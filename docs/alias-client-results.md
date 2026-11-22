# WPA alias client 实验结果

2026-11-23 在本机的独立 SVF worktree 测量，分析实现提交为 `70cb3889`。所有输入使用 Andersen，CUDA 设备为 0，答案块上限为 1,048,576。每个后端在独立进程内生成完整报告，结束后逐字节计算 SHA-256。下表的报告数为不同事件对数。

## 从当前 PTA 到报告

每轮都从 bitcode 启动 WPA。`client_s` 从 PTA 完成后开始，包含构建事件与程序图、规范化索引、选取候选、查询、回传、求解和报告写出。单位为秒。gun 和 gzjoin 各三轮取中位数，tmux 各一轮。

| 输入 | Client | 逻辑 pair | 报告 | CPU | CUDA | Bitmap | Pestrie | CPU/CUDA |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| gun | uaf | 10 | 13 | 0.0026 | 2.2399 | 0.0073 | 0.0073 | 0.001× |
| gun | taint | 9,453 | 93 | 0.0044 | 2.2259 | 0.0090 | 0.0098 | 0.002× |
| gzjoin | uaf | 2,850 | 516 | 0.0031 | 2.2381 | 0.0070 | 0.0082 | 0.001× |
| gzjoin | taint | 4,186 | 459 | 0.0093 | 2.4977 | 0.0148 | 0.0157 | 0.004× |
| tmux | uaf | 357,928,390 | 17,806,721 | 146.6242 | 79.0729 | — | — | 1.854× |
| tmux | taint | 838,676,490 | 378 | 318.9158 | 89.7441 | — | — | 3.554× |

小输入的 GPU 冷启动和索引上传计入每轮时间。tmux 的 UAF 每个命中释放点共享一棵路径树，所有释放–访问对仍写入报告。上下文不敏感、弱更新及分配点抽象会产生大量潜在关系，这些数量不能用来估算独立缺陷数量。

## 同一持久化快照到报告

tmux 的四个后端加载同一份已保存的图、事件和规范化 points-to 分组。Bitmap 使用预构建 alias matrix 查询，Pestrie 使用其原生索引。表中两者均计入从该快照另建并加载自身索引的时间。跨进程复用已有 Bitmap/Pestrie 索引可使用 `-client-index-cache`，是另一种计时口径。每个配置一轮，单位为秒。

| Client | 后端 | 完整 client | 索引与初始化 | 查询与批次传输 | host 消费 | 求解 | 报告编码写入 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| uaf | cpu | 113.905 | 0.005 | 51.829 | 7.151 | 6.368 | 48.186 |
| uaf | cuda | 62.903 | 2.223 | 0.117 | 7.066 | 6.125 | 46.995 |
| uaf | bitmap | 62.719 | 1.407 | 2.275 | 7.048 | 5.909 | 45.726 |
| uaf | pestrie | 62.370 | 1.431 | 2.048 | 7.001 | 5.922 | 45.612 |
| taint | cpu | 241.656 | 0.005 | 182.796 | 13.375 | 43.489 | 0.005 |
| taint | cuda | 61.328 | 2.240 | 0.304 | 13.149 | 43.660 | 0.005 |
| taint | bitmap | 68.652 | 1.419 | 7.660 | 13.385 | 44.238 | 0.003 |
| taint | pestrie | 66.144 | 1.442 | 6.270 | 13.112 | 43.436 | 0.004 |

单独准备 tmux 快照用时 73.58 秒，其中 IR 准备 5.91 秒、PTA 64.35 秒、事件图 0.34 秒、规范化索引 2.84 秒、序列化 0.07 秒。

## 正确性检查

13 组 LLVM bitcode 正负例覆盖释放前后顺序、旧别名、字段、循环、跨过程和间接调用，以及污点跨对象复制、标量传播和未污染的 sink。Andersen 上四个后端分别用 1 和 7 个答案的 tile 与 live 参考报告比对。纯 CPU 构建另以 VFS PTA 通过同组用例。额外的函数定义重排用例核验了两个 client 的事件编号与见证路径。CUDA 边界用例直接抽查了超过 32 位逻辑索引的答案。

gun 和 gzjoin 另行使用 `-client-verify` 将规范化关系逐对与原生 SVF alias 核对，并在快照回放中逐对核对四个后端的答案。tmux 的大规模比对使用实际完整报告的摘要。所有正式计时关闭逐对验证。

## 输入与复现

硬件为双路 AMD EPYC 7542 和一块 NVIDIA RTX PRO 5000 72GB Blackwell，驱动 595.71.05。使用 LLVM 16.0.4、CUDA Toolkit 13.2、Release 加 SVF 断言，CPU 查询单线程。主机共享，各组测量有时间重叠，操作系统文件缓存按常规工作。单轮的 tmux 数据用于定位开销，不足以估计运行间方差。

gun 和 gzjoin 是 Ubuntu zlib1g-dev 1:1.2.11.dfsg-2ubuntu9.2 的命令行程序源码，以 `clang -g -O1 -emit-llvm -c` 编译各自的主翻译单元，外部库调用走已建模摘要或保守外部调用处理。tmux 使用迁移归档内的完整 bitcode。以下为输入 bitcode 的 SHA-256。

- gun 为 `52707ce0f5d83ed187cfcf413e0be9a668219d048222d956bb74a6ae009f650c`。
- gzjoin 为 `8e4a30c82a03cb24394bbfce4049e9f11cf8abcb9e1181eeb848823bb2178251`。
- tmux 为 `1b0a3baa376429efb9d370f37b1b14877399234c539fd42dd50f57ed86b87029`。

可按下列命令复现同一实验设计。目录名用于区分实测与复跑。

```bash
llvm=/data/gzn-data/llvm-16.0.4.obj/bin
mkdir -p build/bench-inputs
for name in gun gzjoin; do
  "$llvm/clang" -g -O1 -emit-llvm -c /usr/share/doc/zlib1g-dev/examples/$name.c -o build/bench-inputs/$name.bc
  python3 tests/alias-clients/benchmark.py build/bench-inputs/$name.bc --wpa Release-build/bin/wpa --live-pta ander --output build/$name-live
done
tar -xOf /data/gzn-data/bc-16-raw.tar bc-16-raw/tmux-3.5a-16.bc > build/bench-inputs/tmux.bc
python3 tests/alias-clients/benchmark.py build/bench-inputs/tmux.bc --wpa Release-build/bin/wpa --live-pta ander --backends cuda cpu --repeats 1 --timeout 600 --output build/tmux-live
Release-build/bin/wpa -ander -client-save=build/bench-inputs/tmux.snapshot build/bench-inputs/tmux.bc
python3 tests/alias-clients/benchmark.py build/bench-inputs/tmux.snapshot --wpa Release-build/bin/wpa --clients taint uaf --repeats 1 --timeout 450 --output build/tmux-replay
```

逐轮原始指标及报告摘要见 [alias-client-measurements.csv](alias-client-measurements.csv)。所有相同输入和 client 的报告摘要一致，包括 tmux 在独立 PTA 进程与快照回放之间的对照。
