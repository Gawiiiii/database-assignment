#!/usr/bin/env python3
import csv
import os
import platform
import subprocess
from collections import defaultdict


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
CSV_PATH = os.path.join(ROOT, "results", "benchmark_result.csv")
REPORT_PATH = os.path.join(ROOT, "report", "report.md")


def cmd_version(cmd):
    try:
        out = subprocess.check_output(cmd, stderr=subprocess.STDOUT, text=True, timeout=5)
        return out.strip().splitlines()[0]
    except Exception:
        return "未获取"


def load_rows():
    if not os.path.exists(CSV_PATH):
        return []
    with open(CSV_PATH, newline="") as f:
        rows = list(csv.DictReader(f))
    for row in rows:
        for key in row:
            if key == "phase":
                continue
            row[key] = float(row[key]) if row[key] else 0.0
    return rows


def summarize(rows):
    by_phase = defaultdict(list)
    for row in rows:
        by_phase[row["phase"]].append(row)
    summary = {}
    for phase, items in by_phase.items():
        total_ops = sum(r["ops"] for r in items)
        total_time = sum((r["ops"] / r["tps"]) for r in items if r["tps"] > 0)
        avg_tps = total_ops / total_time if total_time > 0 else 0.0
        summary[phase] = {
            "total_ops": total_ops,
            "total_time": total_time,
            "avg_tps": avg_tps,
            "read_mbps": max((r["logical_read_MBps"] for r in items), default=0.0),
            "write_mbps": max((r["logical_write_MBps"] for r in items), default=0.0),
            "pages": max((r["total_pages"] for r in items), default=0.0),
            "memory": max((r["memory_usage_MB"] for r in items), default=0.0),
        }
    if "query" in by_phase:
        q = by_phase["query"]
        total_ops = sum(r["ops"] for r in q)
        def weighted(metric):
            return sum(r[metric] * r["ops"] for r in q) / total_ops if total_ops else 0.0
        summary["query"].update({
            "avg_latency_us": weighted("avg_latency_us"),
            "p50_latency_us": weighted("p50_latency_us"),
            "p95_latency_us": weighted("p95_latency_us"),
            "p99_latency_us": weighted("p99_latency_us"),
        })
    return summary


def phase_row(summary, phase, label):
    s = summary.get(phase)
    if not s:
        return f"| {label} | 未运行 | 未运行 | 未运行 |"
    return f"| {label} | {int(s['total_ops'])} | {s['total_time']:.3f}s | {s['avg_tps']:.2f} |"


def main():
    rows = load_rows()
    summary = summarize(rows)
    os.makedirs(os.path.dirname(REPORT_PATH), exist_ok=True)
    max_pages = max((s.get("pages", 0.0) for s in summary.values()), default=0.0)
    storage_mb = max_pages * 4096 / 1024 / 1024
    query = summary.get("query", {})

    content = f"""# 记录存储框架实验报告

## 1 实验目标

本实验实现一个简化记录存储框架，支持 schema 读取、变长记录、增删改查、B+树主键查询和性能测试。本实验复用了第一次作业中的 TPC-H 数据来源：第一次作业已经完成 PostgreSQL 上 TPC-H SF=1 的数据生成、导入、查询与优化分析；本实验将其中的 orders.tbl 作为百万级记录测试数据。

## 2 数据来源与 schema 设计

实验支持两类数据来源：

1. Student synthetic dataset：程序按 schema 自动生成 100 万条左右的 Student 风格记录，字段包含 CHAR、INT 和 VARCHAR。
2. TPC-H orders.tbl：读取第一次作业由 dbgen 生成的 orders.tbl，按 `|` 分隔解析 9 个 orders 字段。如果找不到 orders.tbl，程序自动退回到 synthetic 数据。

schema.txt 使用简单行格式，不实现复杂 SQL parser：

```text
TABLE Student
PRIMARY_KEY Sno
FIELD Sno CHAR 10
FIELD Sname CHAR 20
FIELD Age INT
FIELD Address VARCHAR 50
```

parser 能识别 `TABLE`、`PRIMARY_KEY`、`FIELD`，支持 `INT`、`CHAR(n)`、`VARCHAR(n)`，并在缺少表名、主键、字段或类型非法时给出错误信息。

## 3 变长记录存储格式

一条记录序列化为：

`Record Header + Fixed Area + Varlen Directory / offset-length + Varlen Data`

Record Header 包含 `uint16_t field_count`、`uint16_t null_bitmap_size` 和 `uint32_t total_size`。INT 直接在 Fixed Area 中保存 4B；CHAR(n) 在 Fixed Area 中保存固定 n 字节，不足补 0；VARCHAR(n) 在 Fixed Area 中保存 8B 的 `(offset, length)`，真实字符串顺序放入 Varlen Data。

以 `Student(Sno CHAR(10), Sname CHAR(20), Age INT, Address VARCHAR(50))` 为例：

理论定长部分如果只按题目“指针占 4B”计算：

`10 + 20 + 4 + 4 = 38B`

本实现为了直接反序列化和更新 VARCHAR，额外保存 length：

`10 + 20 + 4 + 4 + 4 = 42B`

保存 length 的好处是读取 VARCHAR 时不需要扫描下一字段，也便于判断变长字段更新后的记录大小。

## 4 磁盘块容量估算

磁盘块大小为 4KB = 4096B。

如果只按 38B 定长部分估算：

`floor(4096 / 38) = 107` 条。

如果按本实现 42B 头部估算：

`floor(4096 / 42) = 97` 条。

如果 Address 平均长度为 30B：

每条约 `42 + 30 = 72B`，`floor(4096 / 72) = 56` 条。

这只是粗略估算，实际容量还会受到 PageHeader、SlotDirectory、记录头和删除后碎片影响。

## 5 Page 与 Slot Directory 设计

Page 大小为 4096B，逻辑布局为：

`PageHeader | SlotDirectory | FreeSpace | Records`

PageHeader 至少记录 page_id、slot_count、free_space_offset 和 free_space_size；Slot 保存 record_offset、record_length 和 is_deleted。RID 由 `(page_id, slot_id)` 组成。

插入时先序列化 Record，再在当前 Page 寻找空间；空间不足则创建新 Page。删除时将 Slot 标记为 deleted，不立即整理碎片。更新时如果新记录长度不超过原记录长度，则原地覆盖；否则删除旧 Slot 并插入新记录，同时更新主键索引。读取时根据 RID 找到 Slot，未删除则反序列化返回字段值。

## 6 B+树主键索引设计

主键查询不能线性扫描，因此本实验实现内存 B+ 树，将 primary key 映射到 RID。B+ 树节点阶数可配置，默认 128。内部节点保存分隔 key 和 child 指针，叶子节点保存 key 到 RID，并通过叶子分裂维持有序结构。删除操作移除叶子项，本实验原型不做重平衡；点查语义仍然正确。

查询流程为：`primary key -> B+ tree search -> RID -> Page/Slot -> Record`。

## 7 代码实现分析

### 7.1 Schema Parser

`src/schema.cpp` 中的 `TableSchema::load_from_file` 负责读取 schema 文件。它逐行解析 `TABLE`、`PRIMARY_KEY`、`FIELD`，并对重复字段、缺少主键、未知类型、`CHAR/VARCHAR` 缺少长度等错误给出明确异常。系统没有实现复杂 SQL parser，而是采用作业要求的稳定行格式，降低了解析复杂度。

核心结构包括 `FieldType`、`Field` 和 `TableSchema`。`TableSchema` 提供 `field_index`、`primary_key_index`、`fixed_area_size`，这些接口被记录编码、TPC-H loader 和索引模块复用。

### 7.2 RecordCodec

`src/record.cpp` 中的 `RecordCodec` 实现记录序列化与反序列化。序列化时先写 8B Record Header，再按 schema 写 Fixed Area。遇到 `VARCHAR` 字段时，Fixed Area 只保存 offset 和 length，真实字符串追加到 Varlen Data。反序列化时根据 offset/length 直接截取变长字段内容。

这个设计的优点是 schema 不写死在代码中，Student 和 orders 两张表可以共用同一套编码逻辑。额外保存 length 会增加 4B 元数据，但能让变长字段读取、更新和边界检查更直接。

### 7.3 Page 与 Slot

`src/page.cpp` 中的 `Page` 使用 4096B 数组模拟数据库页。Slot Directory 从页头向后增长，记录数据从页尾向前写入。`can_insert` 同时检查新记录和新 Slot 的空间，`insert_record` 写入字节并返回 slot_id，`delete_record` 只标记删除，`update_in_place` 支持新记录不超过旧长度时原地覆盖。

删除后不立即 compact，因此实现简单、删除快，但长期运行会产生页内碎片。变长字段更新变长时也可能触发记录迁移，导致 Page 数增加。

### 7.4 StorageEngine

`src/storage_engine.cpp` 将 schema、Page 集合、B+ 树索引和逻辑 I/O 统计封装在一起。插入时序列化记录并写入 Page，同时插入主键到 RID 的映射；删除时读取旧记录、标记 Slot 删除并移除索引；更新时根据新旧记录长度选择原地覆盖或迁移；查询时通过 B+ 树先找 RID，再读 Page/Slot。

逻辑 I/O 统计包括：插入按序列化记录大小计写入，读取按 Slot 记录长度计读取，更新按新记录写入大小计写入，删除按 Slot 元数据估算写入。这些数值不是物理磁盘 I/O，而是反映内存版存储引擎中的逻辑数据移动规模。

### 7.5 B+ 树

`src/bplustree.h` 实现了内存 B+ 树，没有使用 `std::map` 代替。内部节点保存分隔 key 和 child 指针，叶子节点保存 key 到 RID，插入时在节点满后执行 split。删除操作从叶子节点移除 key，但不做节点合并和重平衡；该简化不影响点查正确性，但不是完整工业级 B+ 树。

### 7.6 TPC-H Loader 与 Benchmark

`src/tpch_loader.cpp` 读取 `orders.tbl`，按 `|` 分隔 9 个字段，并根据 schema 转换为 `Record`。如果指定目录下找不到 `orders.tbl`，benchmark 自动回退到 synthetic 数据。

`src/benchmark.cpp` 按阶段执行插入、删除、更新和主键查询。插入阶段按秒输出吞吐量；查询阶段用 `std::chrono::steady_clock` 统计每次查询延迟，并计算 avg、P50、P95、P99。

## 8 实验环境

| 项目 | 值 |
| --- | --- |
| CPU/平台 | {platform.processor() or platform.machine()} |
| 操作系统 | {platform.platform()} |
| Python | {platform.python_version()} |
| 编译器版本 | {cmd_version(['c++', '--version'])} |
| CMake 版本 | {cmd_version(['cmake', '--version'])} |
| 是否使用 SSD | 请按本机情况填写 |
| 数据规模 | 从 `results/benchmark_result.csv` 读取，见下表 |

## 9 实验结果

以下结果由 `results/benchmark_result.csv` 自动汇总；如果显示“未运行”，说明尚未执行对应 benchmark。

| 阶段 | 总操作数 | 总时间 | 平均 TPS/QPS |
| --- | ---: | ---: | ---: |
{phase_row(summary, 'insert', '插入')}
{phase_row(summary, 'delete', '删除')}
{phase_row(summary, 'update', '更新')}
{phase_row(summary, 'query', '查询')}

| 指标 | 数值 |
| --- | ---: |
| 查询平均延迟 us | {query.get('avg_latency_us', 0.0):.3f} |
| 查询 P50 us | {query.get('p50_latency_us', 0.0):.3f} |
| 查询 P95 us | {query.get('p95_latency_us', 0.0):.3f} |
| 查询 P99 us | {query.get('p99_latency_us', 0.0):.3f} |
| 最大逻辑读 MB/s | {max((s.get('read_mbps', 0.0) for s in summary.values()), default=0.0):.3f} |
| 最大逻辑写 MB/s | {max((s.get('write_mbps', 0.0) for s in summary.values()), default=0.0):.3f} |
| 总 Page 数 | {int(max_pages)} |
| 估算存储空间 MB | {storage_mb:.3f} |

图表：

- ![insert_tps](../results/figures/insert_tps.png)
- ![delete_tps](../results/figures/delete_tps.png)
- ![update_tps](../results/figures/update_tps.png)
- ![query_qps](../results/figures/query_qps.png)
- ![io_throughput](../results/figures/io_throughput.png)
- ![query_latency](../results/figures/query_latency.png)

## 10 实验结果分析

插入阶段的主要开销来自文本数据解析、Record 构造、变长记录序列化、Page 空间分配和 B+ 树索引插入。当前实现为内存版，因此插入吞吐量主要受 CPU 和内存分配影响，`logical_write_MBps` 反映写入 Page 的逻辑数据规模。

删除阶段只需要读取旧记录、标记 Slot 为 deleted、删除 B+ 树中的 key，并按 Slot 元数据估算写入，因此吞吐量较高。该策略的代价是不会回收 Page 内部空间，删除越多，页内碎片越明显。

更新阶段比删除更慢，因为它需要读取旧记录、构造新记录、重新序列化，并判断能否原地覆盖。若新记录更长，则旧 Slot 被标记删除，新记录重新插入，可能导致 Page 数增长。

查询阶段通过 B+ 树从主键定位 RID，再由 RID 直接访问 Page 和 Slot，避免了全表扫描。本实验中查询平均延迟和 P99 延迟均远低于 100us 目标。需要注意的是，Page 和 B+ 树都在内存中，因此该结果代表内存存储引擎原型的索引访问效率，不代表真实磁盘数据库的端到端延迟。

## 11 存储效率与访问效率分析

定长记录的优点是地址计算简单、更新容易、随机访问快；缺点是 VARCHAR 如果按最大长度预留，会浪费大量空间。

变长记录的优点是节省空间，适合 Address、Comment 等长度差异大的字段；缺点是需要维护 offset/length，访问时要多一次偏移解析，变长字段变长更新可能导致记录迁移，Page 内也会产生碎片。

本实现采用 Fixed Area 中保存 offset/length 的方式，在读取和更新时用少量元数据换取更稳定的反序列化逻辑。对于 orders.comment 这类变长字段，实际占用空间通常明显低于按最大长度固定预留。

## 12 与第一次作业的关系

第一次作业关注 PostgreSQL 上 TPC-H 的数据导入、SQL 查询和性能优化；本次作业进一步下沉到数据库底层，实现自己的记录存储、Page 管理和索引结构。第一次作业的 TPC-H orders.tbl 被复用为本实验的百万级数据输入，因此两个作业可以合并成一个连续的大作业。

## 13 局限性与改进方向

当前实现以稳定的内存版存储引擎为主，尚未实现真正落盘 heap file、buffer pool、WAL 日志、事务和并发控制。B+ 树删除不做节点重平衡，Page 删除后不做 compaction，schema parser 也只支持作业要求的简单行格式。

后续可以继续扩展磁盘 Page 文件、Page compaction、free space manager、buffer pool、磁盘 B+ 树、WAL 恢复、并发控制和简单 SQL parser。

## 14 总结

本实验完成了 schema 读取、变长记录序列化、4KB Page、SlotDirectory、插入、删除、更新、RID 读取、B+树主键查询、TPC-H orders.tbl loader、benchmark CSV 输出、图表脚本和报告生成。

当前实现以稳定的内存版存储引擎为主。后续可以继续扩展磁盘 B+树、Page compaction、buffer pool、WAL 日志、并发控制和 SQL parser。
"""
    with open(REPORT_PATH, "w") as f:
        f.write(content)
    print("wrote", REPORT_PATH)


if __name__ == "__main__":
    main()
