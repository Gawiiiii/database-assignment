# DB Storage Project

这是一个 C++17 简化数据库记录存储引擎原型，支持 schema.txt 读取、变长记录、4KB Page、SlotDirectory、插入、删除、更新、RID 读取、B+树主键查询、TPC-H orders.tbl 加载、benchmark CSV 输出和图表生成。

## 编译

```bash
mkdir build
cd build
cmake ..
make -j
```

## 运行 synthetic benchmark

```bash
./db_storage_bench \
  --schema ../schema/student.schema.txt \
  --dataset synthetic \
  --limit 1000000 \
  --benchmark all \
  --output ../results/benchmark_result.csv
```

## 运行 TPC-H orders benchmark

`--tpch-dir` 指向包含 `orders.tbl` 的目录。仓库中提供了一个小样本 `data/orders.tbl`，可用于快速验证 TPC-H loader：

```bash
./db_storage_bench \
  --schema ../schema/orders.schema.txt \
  --dataset tpch \
  --tpch-dir ../data \
  --limit 1000 \
  --benchmark all \
  --output ../results/benchmark_result.csv
```

完整百万级 TPC-H 实验需要使用本地 dbgen 生成的完整 `orders.tbl`。由于完整 SF=1 `orders.tbl` 约百 MB 级别，超过 GitHub 普通提交的合理范围，未放入仓库。

```bash
./db_storage_bench \
  --schema ../schema/orders.schema.txt \
  --dataset tpch \
  --tpch-dir /path/to/tpch-gendb \
  --limit 1000000 \
  --benchmark all \
  --output ../results/benchmark_result.csv
```

如果找不到 `orders.tbl`，程序会自动退回到与当前 schema 匹配的 synthetic 数据集。

## 生成图表

```bash
python3 ../scripts/plot_metrics.py ../results/benchmark_result.csv ../results/figures
```

生成的图表包括：

- `insert_tps.png`
- `delete_tps.png`
- `update_tps.png`
- `query_qps.png`
- `io_throughput.png`
- `query_latency.png`

## 单条主键查询接口

程序提供 `--query-key` 作为外部查询接口。它会先按指定 schema 和 dataset 构建内存存储引擎与主键 B+ 树，然后输出主键对应记录；未找到时输出 `NOT_FOUND`。

查询 synthetic Student 数据：

```bash
./db_storage_bench \
  --schema ../schema/student.schema.txt \
  --dataset synthetic \
  --limit 1000 \
  --query-key 0000000005
```

查询 TPC-H orders 小样本：

```bash
./db_storage_bench \
  --schema ../schema/orders.schema.txt \
  --dataset tpch \
  --tpch-dir ../data \
  --limit 1000 \
  --query-key 1
```

输出格式为 `field=value`，字段之间使用 `|` 分隔，便于脚本检查。

## 一键运行

在项目根目录外也可以执行：

```bash
./scripts/run_benchmark.sh synthetic 1000000
./scripts/run_benchmark.sh tpch 1000000 /path/to/tpch-gendb
```

## 实现说明

- 记录格式：`Record Header + Fixed Area + offset/length + Varlen Data`。
- Page 大小：4096B。
- 删除策略：Slot 标记 deleted，不立即 compact。
- 更新策略：新记录不变长则原地覆盖；变长超过旧记录长度则迁移到新 Slot。
- 索引：内存 B+ 树，默认阶数 128，叶子节点保存 primary key 到 RID。
- I/O 统计：当前为逻辑 I/O，按序列化记录长度、Slot 元数据写入和读取记录长度估算。

## 未实现或简化项

- 未实现真正落盘 heap file 和 buffer pool。
- B+树删除不做节点重平衡，点查正确，空间利用率不是优化目标。
- schema parser 只解析本作业要求的简单行格式，不解析 SQL DDL。
