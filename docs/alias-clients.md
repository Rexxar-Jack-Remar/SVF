# WPA 的 UAF 与 taint client

`wpa -alias-client=uaf` 检查释放后可达的内存访问。 `wpa -alias-client=taint` 检查显式数据依赖产生的 source–sink 路径。两者都支持从 bitcode 启动或从完整快照回放。实现、可选 CUDA 后端和两种论文对照均位于本仓库。SVF 原有的 points-to 导出选项仍可独立使用。

## 构建

以下命令适用于迁移机器上的 LLVM 16 和 Z3，需使用 CMake 3.23 或以上版本。示例启用所有后端。省略两个 `SVF_CLIENT_*` 选项和 CUDA 参数可构建纯 CPU 版本。GPU 架构值 120 对应本次实验使用的 Blackwell 设备。

```bash
cmake -S . -B Release-build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DSVF_ENABLE_ASSERTIONS=ON \
  -DLLVM_DIR=/data/gzn-data/llvm-16.0.4.obj/lib/cmake/llvm \
  -DZ3_DIR=/data/gzn-data/z3.obj \
  -DCMAKE_C_COMPILER=/data/gzn-data/llvm-16.0.4.obj/bin/clang \
  -DCMAKE_CXX_COMPILER=/data/gzn-data/llvm-16.0.4.obj/bin/clang++ \
  -DSVF_CLIENT_CUDA=ON -DCMAKE_CUDA_COMPILER=/usr/local/cuda/bin/nvcc \
  -DCMAKE_CUDA_ARCHITECTURES=120 -DSVF_CLIENT_BASELINES=ON
cmake --build Release-build --target wpa -j16
```

## 运行

```bash
wpa=Release-build/bin/wpa
"$wpa" -ander -alias-client=uaf -client-report=uaf.jsonl program.bc
"$wpa" -ander -alias-client=taint -client-backend=cuda -client-device=0 \
  -client-report=taint.jsonl program.bc

"$wpa" -ander -client-save=program.snapshot program.bc
"$wpa" -client-load=program.snapshot -alias-client=uaf -client-backend=cuda \
  -client-tile=1048576 -client-report=uaf-replay.jsonl
"$wpa" -client-load=program.snapshot -alias-client=taint -client-backend=pestrie \
  -client-index-cache=baseline-indexes -client-report=taint-pestrie.jsonl
```

从 bitcode 启动时选择一种 PTA，例如 `-ander` 或 `-vfspta`。快照包含规范化对象池、分组范围、SVF NodeID 映射、控制流、值依赖、事件、API 模型结果和位置。保存采用同目录临时文件和原子替换。回放从该文件恢复完整输入，可在独立进程或另一台有相同后端的机器上运行。

`cpu` 使用规范化集合求交，`cuda` 使用隐式三角索引。`bitmap` 使用 Bitmap 的预构建 alias matrix，`pestrie` 使用 Pestrie 的持久化索引。后两者调用随 WPA 一起构建的 indexer 和原始 `IQuery::IsAlias`，源码取自 `gpu4pta` 提交 `21d73b4` 的 `pestrie/src`，保留原文件头。默认在每次调用中构建并加载这两个索引。指定 `-client-index-cache` 可按规范化输入内容和实现版本复用已写入的索引。`baseline_index` 记录当次的构建或加载状态。

## 查询与分析语义

C++ 入口 `Query::serve(pointers, consume)` 将每个连续区间的逻辑起点与只读布尔答案数组交给 `consume(base, answers)`。回调在下一块复用缓冲区前同步消费这段数据，所有后端使用同一接口。

- SVF 扩展 base 和字段不敏感对象后，相同的非空 points-to 集共用一个分组。已知空集有独立标记。black-hole 和缺失地址映射走保守的 MayAlias 规则。不同事件使用同一 NodeID 时，另行处理该节点的自查询。
- 程序按跨过程控制流的弱连通分量划分查询区域。UAF 提取释放参数与访问地址，taint 提取写入与读取地址。用对侧角色的对象并集筛掉不能参与任何相关边的地址。剩余候选按 NodeID 排序去重。每区独立枚举全部位置对。GPU 复用索引和答案缓冲区，按逻辑任务区间返回逐对答案。host 仅保留有相应读写或释放角色的别名关系。host 保留的关系和报告仍可能很大。
- 控制流从 LLVM 基本块和调用处分段构建，间接调用使用所选 PTA 的已解析 callee，普通调用连接入口和返回续点。循环和递归用图可达性处理。函数可从多个调用点进入，返回边在调用点之间合并。异常调用有保守旁路。分析覆盖模块中的函数体，报告表示上下文不敏感、路径不敏感的潜在关系。
- UAF 识别 `free`、`cfree`、已建模的 C++ delete 以及 `realloc` 的可能释放。访问来自 load、store、原子读改写和函数模型。每条结果要求实际访问在抽象控制流中位于释放之后，且地址别名。指针复制或 GEP 单独出现不会构成解引用。堆对象按 PTA 的抽象粒度处理，循环中不同运行时实例可能合并。
- Taint 为每个 source 传播独立标签，共用同一批别名关系。SSA 值、phi/select、运算、实参/形参及返回值建立有向依赖。污点从一次内存写入传到控制流上随后可达且地址可能重叠的读取。内存采用弱更新，源地址与目标地址不同的复制由摘要显式连接。
- 内存 source 包括 `read/pread/recv/recvfrom` 的缓冲区、`fread/fgets/gets` 的输出、`getenv` 返回的内容和 `scanf/fscanf` 的输出参数。字符输入 `getchar/getc/fgetc` 的返回值产生标量污点。每个 API 仅处理实现声明的参数角色，例如 `recvfrom` 当前处理数据缓冲区。复制覆盖 LLVM memcpy/memmove、常见 C 字符串复制、`strdup` 和 `sscanf` 的输入到输出。`memset` 将填充值写入目标内存。
- Sink 检查 `system/popen/execv/execve` 的命令或路径参数、`puts/fputs/write/send/fwrite` 的数据、`putchar` 的字符以及 `printf/fprintf` 的格式串和实参。嵌套的 `argv/envp` 内容、隐式控制流、线程交错、通用 sanitizer、内部可变参数展开和未知回调超出当前建模范围。
- 未建模外部调用用输入值和可读指针内容影响返回值及可写指针内容，参考调用的 LLVM 内存属性。`unknown_calls` 按 callee 记录静态调用次数。外部隐藏全局副作用及未知 API 自行产生的输入需要额外模型。因此零报告要结合模型覆盖范围解读。

## 报告与计时

`-client-report=FILE` 写 JSON Lines，位置包含函数名、函数内指令序号和操作码，有调试信息时附文件及行号。每个 taint 记录给出 `from`、`to` 和一条包含内存写入及读取依据的 `path`。每个 UAF 记录按释放点汇集所有命中的使用点，避免重复输出大量相同的路径前缀。`uses` 每项为 `[使用点ID, 位置, 路径末端索引]`，`blocks` 每项为 `[前驱索引, 块ID, 入口位置]`。从末端沿前驱反向追至 `-1`，倒序后加上释放点和使用点，即可还原一条抽象路径。UAF 的 `reports` 统计实际的释放点–使用点对数。

stderr 的 `CLIENT_METRICS` JSON 包含耗时、查询数、事件数及结果数。计时单位为秒。

| 字段 | 范围 |
| --- | --- |
| `client_s`，`live-pta` | 从 PTA 完成后的图和事件提取开始，包括索引、候选、分块查询、关系构建、传播和报告关闭 |
| `client_s`，`snapshot` | 从打开已保存的快照开始，包含加载、候选选择、查询、求解和报告。事件提取与 Cactus 索引构建已在快照准备时执行 |
| `total_s`、`ir_s`、`pta_s` | 从 WPA 启动后的入口到报告关闭，以及 IR 准备与指针分析用时。脚本另测子进程完整 wall time |
| `load_s`、`save_s`、`graph_candidates_s`、`index_s` | 快照加载与保存、图和候选准备、后端索引和设备初始化。单独保存快照时输出 `CLIENT_PREPARE` |
| `query_transfer_s`、`consume_s` | 批次提交及逐对查询和回传、host 消费。若开启逐对验证，验证也占用 CPU 时间 |
| `gpu_kernel_s`、`gpu_copy_s` | CUDA event 测得的 kernel 时间，以及上传数组和答案回传的 host 耗时。这些是前述阶段的子项 |
| `solve_s`、`report_s` | 传播、控制流见证构造，以及报告编码和写入。总时间还包含关闭文件、释放资源等开销 |
| `wpa_peak_rss_bytes`、`cuda_buffer_bytes` | Linux 的 WPA 进程峰值 RSS，以及查询后端所持设备缓冲区容量。CUDA context、驱动和 indexer 子进程有各自的内存开销 |

单独的 `-client-verify` 适用于正确性检查。从 bitcode 启动时，它对地址表内包括自查询在内的所有组合调用 SVF 原生 alias 以校验规范化。每次查询时它还比较选定后端的每一条答案。该开销可达二次规模。正式计时通过比较输出文件的 SHA-256 核对结果。

## 验证与复跑

```bash
python3 tests/alias-clients/check.py --wpa Release-build/bin/wpa \
  --llvm-bin /data/gzn-data/llvm-16.0.4.obj/bin --backends cpu cuda bitmap pestrie
cmake --build Release-build --target client-gpu-tiles
Release-build/bin/client-gpu-tiles
python3 tests/alias-clients/benchmark.py program.bc --live-pta ander \
  --wpa Release-build/bin/wpa --output live-runs
python3 tests/alias-clients/benchmark.py program.snapshot \
  --wpa Release-build/bin/wpa --output replay-runs --repeats 3
```

回归用例只编译为 bitcode 后进行静态分析。`client-gpu-tiles` 抽查超过 32 位任务编号的区间、换行边界、空集与未知集合、重复候选和驻留索引上的第二次查询。benchmark 脚本每个配置运行独立进程，轮换后端次序，将超时、失败和报告差异写入 `runs.jsonl`，保留各 client 一份相同报告及所有差异。结果目录须使用新路径，便于保留每次实验的完整记录。
