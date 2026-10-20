"""OpenAI/Anthropic tool envelopes and a model-free client share native tools."""

import importlib.util
import json
import os
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "jash_tools", ROOT / "integrations/jash_tools.py")
jash_tools = importlib.util.module_from_spec(spec)
spec.loader.exec_module(jash_tools)


class AiToolAdaptersTests(unittest.TestCase):
    def test_bounded_provider_callbacks_share_a_live_read_only_loop(self):
        with tempfile.TemporaryDirectory(prefix="jash-tool-loop-") as temporary:
            workspace = Path(temporary)
            directory = workspace / "observed"
            directory.mkdir()
            (directory / "one").touch()
            env = dict(os.environ, HOME=str(workspace / "home"),
                       JASH_SIGIL="0", JASH_NUDGE="0", JASH_BRIEF="0",
                       JASH_STATUSBAR="0", JASH_PROMPT_SPINNER="0",
                       PATH="/usr/bin:/bin", TERM="xterm-256color")
            for adapter in (jash_tools.openai_tool_call,
                            jash_tools.anthropic_tool_use):
                with self.subTest(adapter=adapter.__name__):
                    def provider(prior):
                        if len(prior) == 2:
                            return None
                        operation = "fs.list" if not prior else "fs.observation.read"
                        arguments = ({"root": str(directory)} if not prior else
                                     {"ref": "latest", "offset": 0, "limit": 16})
                        ident = "list" if not prior else "page"
                        if adapter is jash_tools.openai_tool_call:
                            return {"id": ident, "function": {"name": operation,
                                    "arguments": json.dumps(arguments)}}
                        return {"type": "tool_use", "id": ident,
                                "name": operation, "input": arguments}

                    with jash_tools.InteractiveToolSession(
                            ROOT / "build/jash", env=env, cwd=ROOT) as tools:
                        outcomes = jash_tools.run_tool_loop(tools, provider, adapter)
                    self.assertEqual(len(outcomes), 2)
                    self.assertEqual(outcomes[1]["result"]["observation_ref"],
                                     outcomes[0]["result"]["observation_ref"])
                    self.assertEqual(outcomes[1]["result"]["bytes_returned"], 16)

    def test_live_tool_roundtrips_retain_observation_ref(self):
        with tempfile.TemporaryDirectory(prefix="jash-tool-live-") as temporary:
            workspace = Path(temporary)
            directory = workspace / "observed"
            directory.mkdir()
            (directory / "one").touch()
            env = dict(os.environ, HOME=str(workspace / "home"),
                       JASH_SIGIL="0", JASH_NUDGE="0", JASH_BRIEF="0",
                       JASH_STATUSBAR="0", JASH_PROMPT_SPINNER="0",
                       PATH="/usr/bin:/bin", TERM="xterm-256color")
            env.pop("POOLSIDE_API_KEY", None)
            with jash_tools.InteractiveToolSession(
                    ROOT / "build/jash", env=env, cwd=ROOT) as tools:
                openai = tools.call(jash_tools.openai_tool_call({
                    "id": "live-oa", "function": {"name": "fs.count",
                    "arguments": json.dumps({"root": str(directory)})}}))
                anthropic = tools.call(jash_tools.anthropic_tool_use({
                    "type": "tool_use", "id": "live-an", "name": "fs.count",
                    "input": {"root": str(directory)}}))
                self.assertEqual(openai["result"]["items_seen"], 1)
                self.assertEqual(anthropic["result"]["items_seen"], 1)
                listed = tools.call({"version": 1, "id": "live-list",
                                     "op": "fs.list", "root": str(directory)})
                self.assertEqual(listed["result"]["items_seen"], 1)
                page = tools.call({"version": 1, "id": "live-page",
                                   "op": "fs.observation.read", "ref": "latest",
                                   "offset": 0, "limit": 16})
                self.assertEqual(page["result"]["bytes_returned"], 16)
                self.assertEqual(page["result"]["observation_ref"],
                                 listed["result"]["observation_ref"])

    def test_adapters_and_model_free_requests_reuse_read_only_tools(self):
        with tempfile.TemporaryDirectory(prefix="jash-tool-adapters-") as temporary:
            workspace = Path(temporary)
            directory = workspace / "observed"
            directory.mkdir()
            (directory / "one").touch()
            (directory / "two").touch()
            env = dict(os.environ, HOME=str(workspace / "home"),
                       JASH_SIGIL="0", JASH_NUDGE="0", JASH_BRIEF="0",
                       PATH="/usr/bin:/bin")
            env.pop("POOLSIDE_API_KEY", None)
            with jash_tools.ToolSession(ROOT / "build/jash", env=env, cwd=ROOT) as session:
                openai = jash_tools.openai_tool_call({
                    "id": "oa-count", "function": {
                        "name": "fs.count", "arguments":
                        '{"root":' + json.dumps(str(directory)) +
                        ',"type":"file"}'}})
                anthropic = jash_tools.anthropic_tool_use({
                    "type": "tool_use", "id": "an-count", "name": "fs.count",
                    "input": {"root": str(directory), "type": "file"}})
                plain = {"version": 1, "id": "plain-count", "op": "fs.count",
                         "root": str(directory), "type": "file"}
                listing_request = {"version": 1, "id": "list", "op": "fs.list",
                                   "root": str(directory)}
                page_request = {"version": 1, "id": "read",
                                "op": "fs.observation.read", "ref": "latest",
                                "offset": 0, "limit": 16}
                requests = (openai, anthropic, plain, listing_request, page_request)
                answers = session.run(requests)
                for request, answer in zip(requests[:3], answers[:3]):
                    self.assertEqual(answer["result"]["items_seen"], 2)
                    self.assertTrue(answer["result"]["complete"])
                    self.assertEqual(answer["id"], request["id"])
                listing = answers[3]
                self.assertEqual(listing["result"]["items_seen"], 2)
                page = answers[4]
                self.assertEqual(page["result"]["bytes_returned"], 16)
                target = workspace / "should-not-exist"
                with self.assertRaises(jash_tools.ToolError):
                    session.call({"version": 1, "id": "bad", "op": "exec",
                                  "root": f"$(touch {target})"})
                self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
