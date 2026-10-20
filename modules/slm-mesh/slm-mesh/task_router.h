// SPDX-License-Identifier: LicenseRef-SPL-1.0
// task_router.h — Task Routing Header
// Part of Jash Cognitive OS (ADR-0022)

#ifndef TASK_ROUTER_H
#define TASK_ROUTER_H

#include "cloud_bridge.h"

typedef struct {
    cb_provider_t provider;
    const char* model;
    float confidence;
    int is_cloud;
} tr_route_t;

void tr_set_cloud_provider(cb_provider_t provider);
void tr_set_confidence_thresholds(float local, float cloud);
tr_route_t tr_route(const char* task_type, float local_confidence);
void tr_record_route(const tr_route_t* route);
float tr_local_percent(void);
float tr_cloud_percent(void);
void tr_reset_stats(void);

#endif