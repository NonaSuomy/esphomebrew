#pragma once

#include "psram_app.h"
#include "papp2_abi.h"

void papp2_runtime_begin(const app_services_t *services);
void papp2_runtime_end();
const papp_if_audi_t *papp2_runtime_audi();
const papp_if_inpt_t *papp2_runtime_inpt();
const papp_if_thrd_t *papp2_runtime_thrd();
const papp_if_ui_t *papp2_runtime_ui();
