#pragma once

#include "cmt_types.h"

void update_system_run_permissions();
void handle_remote_change_event(RemoteState new_state);
void scan_remote_hardware();
void init_cores_communication();
