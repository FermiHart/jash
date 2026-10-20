#include "minix/surface.h"
#include "surface.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int status_calls;
static int consume_calls;
static int wait_calls;
static int ack_calls;
static int emit_calls;
static uint32_t emitted_type[8];
static uint32_t emitted_len[8];
static char emitted[8][SURFACE_EMIT_INLINE + 1];
static const char *input_payload;
static int input_ready;

static void fd_text(int fd, char out[16])
{
    char reverse[16];
    size_t n = 0;
    unsigned value = (unsigned)fd;
    do {
        reverse[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n < sizeof(reverse));
    for (size_t i = 0; i < n; i++) out[i] = reverse[n - i - 1];
    out[n] = '\0';
}

int bear_surface_reader_op(uint32_t session, struct surface_reader *reader)
{
    assert(session == 17 && reader->role == SURFACE_READER_INPUT);
    if (reader->action == SURFACE_READER_STATUS) {
        status_calls++;
        reader->endpoint = 123;
        reader->epoch = 99;
        reader->next_seq = 1; /* output PROMPT cells precede INPUT seq 40 */
    } else {
        assert(reader->action == SURFACE_READER_ACK);
        assert(reader->endpoint == 123 && reader->epoch == 99);
        assert(reader->next_seq == 41);
        ack_calls++;
    }
    return 0;
}

int bear_surface_consume(uint32_t session, struct surface_reader *reader,
                         struct surface_take256 *takes, struct surface_cell_ref *refs,
                         uint32_t cap)
{
    assert(session == 17 && cap == 1 && reader->epoch == 99);
    consume_calls++;
    if (!input_ready) return 0;
    size_t len = strlen(input_payload);
    assert(len <= SURFACE_EMIT_INLINE);
    memset(takes, 0, sizeof(*takes));
    memset(refs, 0, sizeof(*refs));
    takes[0].hdr.seq = 40;
    takes[0].hdr.cell_type = SURFACE_CELL_INPUT;
    takes[0].hdr.len = (uint16_t)len;
    takes[0].hdr.session = 17;
    takes[0].hdr.endpoint = 456;
    memcpy(takes[0].data, input_payload, len);
    refs[0].cell_type = SURFACE_CELL_INPUT;
    refs[0].origin = SURFACE_ORIGIN_PROGRAM;
    refs[0].len = (uint16_t)len;
    refs[0].seq = 40;
    refs[0].rendered_count = 1;
    refs[0].pid = 456;
    refs[0].session = 17;
    reader->next_seq = 41;
    return 1;
}

int bear_surface_consumer_wait(uint32_t session, const struct surface_reader *reader)
{
    assert(session == 17 && reader->epoch == 99);
    wait_calls++;
    input_ready = 1;
    return 0;
}

int bear_surface_emit_wait(uint32_t type, const void *data, uint32_t len)
{
    assert(len > 0 && len <= SURFACE_EMIT_INLINE && emit_calls < 8);
    emitted_type[emit_calls] = type;
    emitted_len[emit_calls] = len;
    memcpy(emitted[emit_calls], data, len);
    emitted[emit_calls][len] = '\0';
    emit_calls++;
    return (int)len;
}

int main(void)
{
    struct jash_minix_surface surface;
    char line[128];
    char large[300];
    char captured[300];
    char control;
    int flush_pipe[2], ack_pipe[2], output_pipe[2];
    int saved_stdout;

    assert(pipe(flush_pipe) == 0 && pipe(ack_pipe) == 0);
    assert(fcntl(flush_pipe[0], F_SETFL, O_NONBLOCK) == 0);
    char flush_fd[16], ack_fd[16];
    fd_text(flush_pipe[1], flush_fd);
    fd_text(ack_pipe[0], ack_fd);
    assert(setenv("JASH_SURFACE_FLUSH_FD", flush_fd, 1) == 0);
    assert(setenv("JASH_SURFACE_ACK_FD", ack_fd, 1) == 0);

    memset(&surface, 0, sizeof(surface));
    assert(jash_minix_surface_open("console.17", &surface) == 0);
    assert((fcntl(flush_pipe[1], F_GETFD) & FD_CLOEXEC) != 0);
    assert((fcntl(ack_pipe[0], F_GETFD) & FD_CLOEXEC) != 0);
    assert(status_calls == 1 && surface.session == 17);
    assert(surface.input.endpoint == 123 && surface.input.epoch == 99);
    assert(jash_minix_surface_open("console.0", &surface) < 0);
    assert(jash_minix_surface_open("console.17x", &surface) < 0);

    input_payload = "console.17:echo ok";
    input_ready = 0;
    assert(jash_minix_surface_readline(&surface, line, sizeof(line)) == 7);
    assert(strcmp(line, "echo ok") == 0);
    assert(wait_calls == 1 && consume_calls == 2 && ack_calls == 1);

    assert(write(ack_pipe[1], "A", 1) == 1);
    assert(jash_minix_surface_prompt(&surface) == 14);
    assert(read(flush_pipe[0], &control, 1) == 1 && control == 'F');
    assert(emitted_type[0] == SURFACE_CELL_PROMPT);
    assert(strcmp(emitted[0], "jash:surface$ ") == 0);
    assert(write(ack_pipe[1], "A", 1) == 1);
    assert(jash_minix_surface_receipt(&surface, 7) == 0);
    assert(read(flush_pipe[0], &control, 1) == 1 && control == 'F');
    assert(emitted_type[1] == SURFACE_CELL_KV);
    assert(strcmp(emitted[1], "jash.status tag=console.17 index=1 status=7\n") == 0);
    assert(surface.command_index == 1);

    memset(large, 'x', sizeof(large));
    assert(pipe(output_pipe) == 0);
    saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0 && dup2(output_pipe[1], STDOUT_FILENO) == STDOUT_FILENO);
    close(output_pipe[1]);
    assert(jash_minix_surface_writer(&surface, large, sizeof(large)) == (long)sizeof(large));
    assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    close(saved_stdout);
    size_t captured_len = 0;
    while (captured_len < sizeof(captured)) {
        ssize_t n = read(output_pipe[0], captured + captured_len,
            sizeof(captured) - captured_len);
        assert(n > 0);
        captured_len += (size_t)n;
    }
    assert(memcmp(captured, large, sizeof(large)) == 0);
    close(output_pipe[0]);
    assert(emit_calls == 2);

    return 0;
}
