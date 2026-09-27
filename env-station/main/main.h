#pragma once

/* 控制台行分发：USB 控制台任务与 WFP TCP 端点（wfp_tcp.c）共用。 */
void cmd_dispatch(const char *line);

/* 最新温湿度（校准后显示值）；尚无读数返回 false。供 Web 状态页读取。 */
bool env_latest(float *t, float *rh);
