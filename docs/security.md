# 安全特性文档

## 目录

1. [安全威胁模型](#1-安全威胁模型)
2. [调试防护](#2-调试防护)
3. [加密内存](#3-加密内存)
4. [ASan 集成](#4-asan-集成)
5. [安全配置示例](#5-安全配置示例)
6. [安全最佳实践](#6-安全最佳实践)
7. [合规性](#7-合规性)

---

## 1. 安全威胁模型

### 1.1 威胁分类

| 威胁 | 等级 | 防护措施 |
| :--- | :--- | :--- |
| 缓冲区溢出 | 🔴 高 | Canary、Guard Pages、ASan |
| Use-After-Free | 🔴 高 | Poison On Free、ASan |
| 双 Free / 野指针 | 🟠 中高 | Magic 校验、ASan |
| 内存泄漏 | 🟡 中 | 泄漏检测、审计 |
| 敏感数据泄露 | 🔴 高 | 加密内存、secure zero |
| 侧信道攻击 | 🟡 中 | 随机化、mlock |
| 拒绝服务 (OOM) | 🟠 中高 | 内存限制、熔断器 |

### 1.2 攻击面

```
+------------------+
|   Application    |
+--------+---------+
         |
+--------v---------+
|   cmem Pool      | <-- 攻击面：恶意输入导致溢出/UAF
+--------+---------+
         |
+--------v---------+
|   OS Kernel       |
+------------------+
```

---

## 2. 调试防护

### 2.1 Canary 越界检测

**实现：**
```c
// 分配时在 payload 后填充 canary
if (pool->flags & MP_FLAG_DEBUG_CANARY) {
    uint8_t* canary = (uint8_t*)ptr + size;
    *canary = MP_CANARY_BYTE;  // 0xAB
}

// 释放时校验 canary
if (pool->flags & MP_FLAG_DEBUG_CANARY) {
    uint8_t* canary = (uint8_t*)ptr + header->requested_size;
    if (*canary != MP_CANARY_BYTE) {
        // Buffer overflow detected!
        mp_mark_pool_dirty(pool);
    }
}
```

**开销：**
- 内存：每块 +1 字节
- 性能：~3%  slowdown

### 2.2 UAF 毒化

**实现：**
```c
// 释放时填充毒化字节
if (pool->flags & MP_FLAG_POISON_ON_FREE) {
    memset(ptr, 0xDD, header->usable_size);
}
```

**效果：**
- 释放后访问会看到 `0xDDDDDDDD` 模式
- 便于调试 Use-After-Free

### 2.3 Guard Pages

**实现：**
```c
// 在页首/页尾设置 PROT_NONE
if (pool->flags & MP_FLAG_GUARD_PAGES) {
    mprotect(page_addr, page_size, PROT_NONE);
}
```

**效果：**
- 越界访问触发 SIGSEGV
- 页对齐浪费（每块 +2 页）

### 2.4 双 Free 检测

**实现：**
```c
// 块头 Magic 校验
if (header->magic != MP_MAGIC_HEAD) {
    fprintf(stderr, "[ERROR] Corrupt header or invalid free!\n");
    mp_mark_pool_dirty(pool);
    return;
}
```

---

## 3. 加密内存

### 3.1 mlock 防 Swap

```c
int mp_lock_memory(memory_pool_t* pool, void* addr, size_t length) {
#ifdef __linux__
    if (mlock(addr, length) != 0) {
        perror("mlock failed");
        return -1;
    }
#endif
    return 0;
}
```

**安全收益：**
- 敏感数据不写入 swap 分区
- 防止冷启动攻击

**限制：**
- 每个进程有锁内存上限（通常 64KB~1GB）
- 需要 `CAP_IPC_LOCK` 或提高 `ulimit -l`

### 3.2 MADV_DONTDUMP 防 Core Dump

```c
int mp_protect_from_dump(memory_pool_t* pool, void* addr, size_t length) {
#ifdef __linux__
    if (madvise(addr, length, MADV_DONTDUMP) != 0) {
        perror("madvise failed");
        return -1;
    }
#endif
    return 0;
}
```

**安全收益：**
- 敏感数据不出现在 core dump 中
- 防止调试器提取密钥

### 3.3 Secure Zero

```c
void mp_secure_zero(memory_pool_t* pool, void* ptr, size_t length) {
    volatile unsigned char* p = (volatile unsigned char*)ptr;
    while (length--) {
        *p++ = 0;
    }
}
```

**关键点：**
- 使用 `volatile` 防止编译器优化
- 确保清零操作实际执行

### 3.4 加密内存模式

```c
// 一键启用加密内存
mp_set_encrypted_memory(pool, true);

// 自动对系统分配的内存执行：
// 1. mlock 锁定
// 2. MADV_DONTDUMP 排除
```

---

## 4. ASan 集成

### 4.1 检测能力

| 检测项 | ASan | cmem 原生 | 组合效果 |
| :--- | :--- | :--- | :--- |
| 堆溢出 | ✅ | ✅ | ✅✅ |
| 栈溢出 | ✅ | ❌ | ✅ |
| 全局溢出 | ✅ | ❌ | ✅ |
| Use-After-Free | ✅ | ✅ | ✅✅ |
| 双 Free | ✅ | ✅ | ✅✅ |
| 内存泄漏 | ✅ | ✅ | ✅✅ |

### 4.2 集成模式

```c
// 启用 ASan 集成模式
mp_set_asan_integration(pool, true);

// 检测 ASan 环境
if (mp_asan_is_enabled()) {
    // 调整策略以兼容 ASan
}
```

### 4.3 自定义错误报告

```c
// 检测到 canary 越界后报告给 ASan
if (*canary != MP_CANARY_BYTE) {
    mp_asan_report_error(pool, ptr, size, true);
}
```

---

## 5. 安全配置示例

### 5.1 最高安全级别

```c
mp_flags_t flags = MP_FLAG_THREAD_SAFE |
                   MP_FLAG_DEBUG_CANARY |
                   MP_FLAG_POISON_ON_FREE |
                   MP_FLAG_TRACK_LOCATIONS |
                   MP_FLAG_GUARD_PAGES |
                   MP_FLAG_ENCRYPTED_MEMORY |
                   MP_FLAG_ASAN_INTEGRATION;

memory_pool_t* pool = mp_create(64 * 1024 * 1024, flags);
mp_set_memory_limit(pool, 128 * 1024 * 1024);
mp_set_circuit_breaker(pool, true);
mp_set_thread_quota(pool, 8 * 1024 * 1024);
```

**适用场景：**
- 安全敏感应用
- 密码学库
- 金融交易系统

### 5.2 平衡安全与性能

```c
mp_flags_t flags = MP_FLAG_THREAD_SAFE |
                   MP_FLAG_DEBUG_CANARY |
                   MP_FLAG_POISON_ON_FREE;

memory_pool_t* pool = mp_create(64 * 1024 * 1024, flags);
mp_set_memory_limit(pool, 128 * 1024 * 1024);
```

**适用场景：**
- 生产环境
- 需要一定调试能力

### 5.3 最小安全配置

```c
mp_flags_t flags = MP_FLAG_THREAD_SAFE;

memory_pool_t* pool = mp_create(64 * 1024 * 1024, flags);
mp_set_memory_limit(pool, 128 * 1024 * 1024);
```

**适用场景：**
- 性能关键路径
- 可信输入环境

---

## 6. 安全最佳实践

### 6.1 开发阶段

```c
// 启用所有调试特性
mp_flags_t flags = MP_FLAG_THREAD_SAFE |
                   MP_FLAG_DEBUG_CANARY |
                   MP_FLAG_POISON_ON_FREE |
                   MP_FLAG_TRACK_LOCATIONS;

memory_pool_t* pool = mp_create(0, flags);

// 定期审计
bool healthy = mp_audit_heap(pool);
assert(healthy);

// 检查泄漏
assert(mp_check_leaks(pool));
```

### 6.2 生产环境

```c
// 选择合适的安全级别
mp_flags_t flags = MP_FLAG_THREAD_SAFE |
                   MP_FLAG_DEBUG_CANARY;

memory_pool_t* pool = mp_create(0, flags);

// 设置内存限制
mp_set_memory_limit(pool, 512 * 1024 * 1024);

// 启用熔断器
mp_set_circuit_breaker(pool, true);
mp_set_thread_quota(pool, 64 * 1024 * 1024);

// 监控异常
mp_set_event_callback(pool, security_event_cb, NULL);
```

### 6.3 安全事件回调

```c
void security_event_cb(memory_pool_t* pool, mp_event_type_t ev,
                       void* ptr, size_t size, void* user_data) {
    switch (ev) {
        case MP_EVENT_CANARY_CORRUPTION:
            log_security_event("Buffer overflow detected");
            mp_mark_pool_dirty(pool);
            break;
        case MP_EVENT_DOUBLE_FREE:
            log_security_event("Double free detected");
            break;
        case MP_EVENT_OOM:
            log_security_event("Out of memory");
            break;
        default:
            break;
    }
}
```

---

## 7. 合规性

### 7.1 PCI DSS

- 要求：持卡人数据加密存储
- 实现：`MP_FLAG_ENCRYPTED_MEMORY` + `mp_secure_zero`

### 7.2 GDPR

- 要求：个人数据保护
- 实现：加密内存 + 安全清零 + 泄漏检测

### 7.3 HIPAA

- 要求：医疗数据保护
- 实现：mlock + MADV_DONTDUMP + 审计日志

### 7.4 SOC 2

- 要求：访问控制、加密、审计
- 实现：完整安全 Flag 组合 + 事件回调 + Prometheus 导出

---

## 附录：安全特性 checklist

- [ ] 根据场景选择适当的安全 Flag 组合
- [ ] 开发环境启用 `MP_FLAG_TRACK_LOCATIONS`
- [ ] 生产环境启用 `MP_FLAG_DEBUG_CANARY`
- [ ] 敏感数据场景启用 `MP_FLAG_ENCRYPTED_MEMORY`
- [ ] 配置 `mp_set_memory_limit` 防止 OOM 攻击
- [ ] 启用 `mp_set_circuit_breaker` 防止资源耗尽
- [ ] 注册安全事件回调
- [ ] 定期运行 `mp_audit_heap` 和 `mp_check_leaks`
- [ ] 导出 Prometheus 指标进行安全监控
- [ ] 定期安全审计和渗透测试
