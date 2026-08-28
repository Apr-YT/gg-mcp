# 代码重构指南

## search.c 重构

### 当前问题
- 代码重复35%
- 缺少NULL检查
- 函数过长

### 重构步骤

1. 提取通用函数
```c
// 将重复的搜索逻辑提取
int search_common(...) {
    // 统一的搜索实现
}

// 在各类型搜索中调用
int search_dword(...) {
    return search_common(...);
}
```

2. 添加NULL检查
``c
// 修复缺失的检查
uint8_t *data = malloc(size);
if (!data) {
    log_error("malloc failed");
    return -1;
}
```

3. 拆分大函数
- 保持函数 < 200行
- 提取子函数
- 减少嵌套深度 < 5

## watch.c 重构

### 当前问题
- 圈复杂度68
- 错误处理不完整

### 重构步骤

1. 降低复杂度
- 提取子函数
- 使用早返回
- 减少嵌套

2. 增强错误处理
- 所有系统调用检查返回值
- 添加错误日志

## 错误处理统一

使用log_enhance.h:
``c
#include "log_enhance.h"

// 统一错误返回
RETURN_ERROR(GG_NOMEM, "Memory allocation failed");

// NULL检查
CHECK_NULL(ptr, cleanup);

// 安全分配
SAFE_MALLOC(ptr, size, cleanup);
```
