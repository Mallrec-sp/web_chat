#pragma once

// NetChat Logger 封装
// 基于 Log 库，提供项目统一的日志接口
//
// 使用方式:
//   LOG_DEBUG("connect from " << addr << ":" << port);
//   LOG_INFO("server started on port " << port);
//   LOG_WARN("connection timeout: fd=" << fd);
//   LOG_ERROR("accept failed: " << strerror(errno));

#include "log.h"

// 保留原有的 Log 宏直接可用
// LOG_DEBUG / LOG_INFO / LOG_WARNING / LOG_ERROR
//
// 后续优化方向 (当遇到性能瓶颈时):
//   1. 改为基于 fmtlib 的格式化，减少 string 临时对象
//   2. 加入编译期日志级别过滤
//   3. 控制台输出移出锁
