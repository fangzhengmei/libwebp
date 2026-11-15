# VP8L 哈夫曼表构建上限分析

分析对象：本仓库 `src/dec/vp8l_dec.c`、`src/utils/huffman_utils.c`、`src/utils/color_cache_utils.c`（libwebp 1.6.0，commit `7c382bd`）。
威胁模型：攻击者提交一个 VP8L 无损 WebP。格式允许用 `code_length` 描述任意前缀码表，用 `meta huffman` 把图像切成最多 65536 个哈夫曼组，用 `color cache` 把绿色字母表从 280 拉宽到 2328。这三者相乘，就是本文要量化的表构建上限。

本文的结论汇总：

- 单表的构建有严格、正确的合法性检查（不完整表、过度订阅、超长码长都会被拒绝），且单表大小有一个可实测的上界，与 `kTableSize` 一致。
- 真正缺失的是**总量上限**：组数与表规模两个维度各自合法，但乘起来没有边界。295 KB 的输入可以让解码器分配 336 MB 哈夫曼表（1139×）；单组层面，98 B 的最小合法表也能换到 20016 B 的表（204×）。
- **彩色缓存**是放大的乘数：`cc=11` 时每组预算从 11816 B 增至 20016 B（1.7×），而 `kTableSize` 本身仍然安全。
- 建议的修复是按 `MAX_HUFF_IMAGE_SIZE × kTableSize[11]` 设一个累计条目预算，并在实测中确认：合法编码器输出的 40 个样本全部像素一致通过，而 65536 组病理样本被干净拒绝（RSS 336 MB → 54 MB）。

---

## 1. 相关代码路径与边界点

### 1.1 码长读取

`ReadHuffmanCode()`（`src/dec/vp8l_dec.c:334`）先读 1 bit 判断 simple/full：

- **simple 码**：直接写 `code_lengths[symbol] = 1`，最多两个符号。注意这里 `symbol` 可以超过 `alphabet_size`（注释在 `vp8l_dec.c:345` 说明），但 `code_lengths[]` 是按 `max_alphabet_size` 分配的，所以不会越界；`BuildHuffmanTable` 只遍历 `code_lengths_size` 项，简单码里超出的符号被忽略。
- **full 码**：调用 `ReadHuffmanCodeLengths()`（`vp8l_dec.c:263`），它先用 19 项 `code_length_code_lengths` 建**码长码**的表，再用该表读 `num_symbols` 个码长。

边界 1（码长码自身的表）：`VP8LHuffmanTablesAllocate(1 << LENGTHS_TABLE_BITS, ...)`（`vp8l_dec.c:279`）只预分配 **128** 个 `HuffmanCode`。这个表是否可能超过 128？

- 码长码的 19 个码长值来自 3 bit 字段（`code_length_code_lengths[...] = VP8LReadBits(br, 3)`，`vp8l_dec.c:364`），因此每个都 ≤ 7。
- 因为 `root_bits = LENGTHS_TABLE_BITS = 7`，所有码长 ≤ 7 时不存在二级子表，`BuildHuffmanTable` 只可能返回 `1 << 7 = 128` 或单符号特例的 128。
- 实测（200 万次随机完整码采样的最坏值）：`max_table=128`，恰好等于预分配。

所以码长表这一层是硬封顶的，不会因超长码长而膨胀。

边界 2（码长展开）：`ReadHuffmanCodeLengths()` 检查 `symbol + repeat > num_symbols` 后 `goto End`（`vp8l_dec.c:320`），repeat 计数不会写越 `code_lengths`。

### 1.2 表大小预估（`kTableSize`）

`src/dec/vp8l_dec.c:85-90`：

```c
#define FIXED_TABLE_SIZE (630 * 3 + 410)     // = 2300
static const uint16_t kTableSize[12] = {
    FIXED_TABLE_SIZE + 654, ... FIXED_TABLE_SIZE + 2704};
```

注释（`vp8l_dec.c:77-84`）说明这是"红/蓝/Alpha 的 256 字符表最坏 630，距离表最坏 410，绿色表取决于缓存"。注意 `kTableSize[cc]` 是**一组 5 棵树之和**的上界，而不是单棵树的上界：

- `cc=0`：`FIXED + 654 = 2954 = 654 + 3*630 + 410`。
- `cc=11`：`5004 = 2704 + 3*630 + 410`。

本文用随机"保持 Kraft 完全性"的树扩展搜索，实测各字母表的单表最坏值（`BuildHuffmanTable` 真实返回值）：

| cc | 绿色字母表 | kTableSize[cc] | 实测绿色表最坏 | 余量 |
|---|---|---|---|---|
| 0 | 280 | 2954 | 598 | 56 |
| 5 | 312 | 2986 | 622 | 64 |
| 8 | 536 | 3212 | 844 | 68 |
| 11 | 2328 | 5004 | 2638 | 66 |

同时实测红/蓝/Alpha（字母表 256）最坏 582，距离表（40）最坏 406，都低于 630/410 的常数。**结论：`kTableSize` 是与真实构建器一致的合法上界，不是本次的问题所在。** 它的问题在于只被用来"预估一次分配"，没有被用来"限制实际构建总量"。

### 1.3 主表构建与子表构建

`BuildHuffmanTable()`（`src/utils/huffman_utils.c:88`）的合法性检查是完备的：

- `code_lengths[symbol] > MAX_ALLOWED_CODE_LENGTH(15)` → 返回 0（`huffman_utils.c:111`）。覆盖"超长码长"。
- 全 0 → 返回 0（`huffman_utils.c:119`）。
- `count[len] > (1 << len)` → 返回 0（`huffman_utils.c:124`）。这是快速过度订阅检查。
- 根层填充时 `num_open < 0` → 返回 0（`huffman_utils.c:180`）。覆盖更深层的过度订阅。
- 最终 `num_nodes != 2 * offset[MAX] - 1` → 返回 0（`huffman_utils.c:221`）。覆盖**不完整表**（Kraft 和 < 1）。

二级子表宽度由 `NextTableBitSize()`（`huffman_utils.c:73`）决定，宽度 1..7，每个二级表 2..128 项。这部分逻辑本身有界，但它意味着**表大小是码长分布的强函数**：把大量短码换成深层完整码，可以在字母表不变的情况下把表从 256 抬到 600 左右。这就是"合法但接近上限"的构造空间。

一个容易误读的点：`VP8LBuildHuffmanTable()` 先调用 `BuildHuffmanTable(NULL, ...)` 只算大小，再真正填充（`huffman_utils.c:271-296`）。这意味着**每个不合法的树也要先被完整遍历一遍**才返回 0，CPU 成本可被恶意树放大（见 §4 CPU 数据）。

### 1.4 颜色缓存配置

`DecodeImageStream()`（`vp8l_dec.c:1841-1870`）：

```c
color_cache_bits = VP8LReadBits(br, 4);
ok = (color_cache_bits >= 1 && color_cache_bits <= MAX_CACHE_BITS);
...
hdr->color_cache_size = 1 << color_cache_bits;
VP8LColorCacheInit(&hdr->color_cache, color_cache_bits);   // calloc(1<<bits, 4)
```

范围检查正确（1..11）。它影响两处：

- 缓存本身：最多 `1<<11 = 2048` 个 `uint32_t`，8 KB，可忽略。
- **绿色字母表**：`ReadHuffmanCodesHelper()` 里 `alphabet_size += (1 << color_cache_bits)`（`vp8l_dec.c:535`），使绿色表从 280 涨到 2328，单组预算从 11816 B 涨到 20016 B。

因此颜色缓存是**合法的表放大乘数**，不是独立的漏洞：当 `cc` 从 0 变到 11，同样的组数能多分配 1.7× 内存。

### 1.5 组数与分配策略

`ReadHuffmanCodes()`（`vp8l_dec.c:386`）：

- meta huffman 图像每个像素的 `(>>8)&0xffff` 是组索引，`num_htree_groups_max = max + 1`，格式硬上限 65536。
- `num_htree_groups_max > 200` 时建立映射，把未使用的索引映射掉（`vp8l_dec.c:431`）。**但重要事实：即使建了映射，`for (i = 0; i < num_htree_groups_max; ++i)` 仍然会为每个未使用索引解析 5 棵树**（`vp8l_dec.c:528-541`，传入 `NULL` 表，不分配但不免除解析）。这造成一个纯 CPU 放大路径，与内存无关。
- `htree_groups = VP8LHtreeGroupsNew(num_htree_groups)`（仅按**使用过的**组数分配，`huffman_utils.c:31`，每组约 56 B）。
- 表内存：`VP8LHuffmanTablesAllocate(total_huffman_table_size, ...)` 初始只分配一个 chunk，之后 `VP8LHuffmanTablesEnsureCapacity()` 按需追加 segment（`huffman_utils.c:236`），**总量无上限**。

关键的"缺失边界"就在这里：`num_htree_groups × table_size` 只在初始 chunk 里被当作"一次预分配大小"（`vp8l_dec.c:516-520`），但真正逐组构建时没有任何累计检查。每个组的表大小都必须 ≤ 各自的字母表上界，这个是对的；但 65536 个组累加就没有边界了。

---

## 2. 实测：边界行为

工具位于仓库外（`/tmp/vp8lab`、`/tmp/huffprobe`），由三部分组成：

- `vp8lgen.py` / `multigroup.py`：LSB-first 最小 VP8L 位流生成器，可精确写出 simple/full 码、meta huffman 组、颜色缓存。已用一个独立 Python 解码镜像（`decmirror.py`）逐位验证 round-trip。
- `measure.c`：只调 `VP8LDecodeHeader()`，把表构建从像素解码中隔离；用 `--wrap=VP8LBuildHuffmanTable` 记录每次构建。
- `harness.c` / `pixelcmp.c` / `trunc.c`：完整解码、像素 CRC、逐字节截断扫描。

### 2.1 合法性检查矩阵（单组，1×1 图像）

所有输入都是真实可解码的位流（除标注 error 的行），`status` 是 `VP8StatusCode`（0=OK，3=BITSTREAM_ERROR）。

| 样本 | 构造 | status | 表字节 | 耗时 ms | 峰值 RSS KB |
|---|---|---|---|---|---|
| A_legal_complete_cc0 | 256×len8 完整绿表 | 0 | 11816 | 0.015 | 1968 |
| B_legal_maxlen_cc0 | 深完整表（3 个 128 项二级表） | 0 | 11816 | 0.016 | 1976 |
| B_legal_maxlen_cc11 | 深完整表 + cc=11 | 0 | 20016 | 0.045 | 1980 |
| C_incomplete_2x2 | 2 个 len2（Kraft=1/2） | **3** | 0 | 0.005 | 1980 |
| D_incomplete_1p3 | len1 + len3 | **3** | 0 | 0.005 | 1980 |
| E_oversub_3x1 | 3 个 len1 | **3** | 0 | 0.006 | 1980 |
| F_oversub_5x2 | 5 个 len2 | **3** | 0 | 0.005 | 1980 |
| G_cache_bits12 | 缓存位=12 | **3** | 0 | 0.001 | 1980 |
| H_cache_bits15 | 缓存位=15 | **3** | 0 | 0.001 | 1980 |

超长码长（`len16`）在 `BuildHuffmanTable` 里返回 0（§1.3），构造同样得到 status=3。**不完整表 / 过度订阅 / 超长码长都在构建器内部被拒绝，没有内存副作用。**

### 2.2 合法表的成本上界（单组）

每组 5 棵树，`cc=0` 时一组最坏 11816 B（= 2954 codes × 4），`cc=11` 时 20016 B（= 5004 × 4）。实测最深合法构造：

- 绿色 2328 字母表 + 256 个二级前缀构造 → 单树 2304 codes。
- 5 棵树合计 4790 codes，仍在 5004 之内。

即使只有**一个**组，输入/表字节比也很可观——因为 simple 码几乎是零成本：

| 样本 | 输入字节 | 表字节 | 放大 |
|---|---|---|---|
| cheap_g8_256x8（绿表 256×len8，cc=11） | 98 | 20 016 | 204× |
| cheap_g11_2048x11（绿表 2048×len11，cc=11） | 340 | 20 016 | 59× |

这是"表大小由码长分布的合法性决定、而不是由输入比特数决定"的直接体现：`cc=11` 让单组的合法预算固定为 20016 B，而写出一个合法表所需的比特可以很少。

### 2.3 组数放大（核心问题）

同一台机器，同样的解码路径，只改变组数：

| 样本 | 输入字节 | 组数 | 表字节 | 耗时 ms | 峰值 RSS KB |
|---|---|---|---|---|---|
| g1_cc0_simple | 132 | 1 | 11 816 | 0.017 | 1 940 |
| g200_cc0_simple | 131 692 | 200 | 1 134 336 | 1.86 | 3 200 |
| g1000_cc0_simple | 133 692 | 1 000 | 5 529 888 | 5.60 | 7 040 |
| amp_1000_deep_cc11 | 1 347 640 | 1 000 | 13 530 816 | 36.4 | 16 000 |
| g2600_deep11 | 3 549 544 | 2 600 | 35 388 288 | 91.7 | 39 424 |
| thr_10164 | 549 818 | 10 164 | 52 226 720 | 53.0 | 55 168 |
| **amp_65536_simple** | **295 032** | **65 536** | **336 094 304** | **312.8** | **336 384** |

最后一行是最小输入的病理样本：**295 KB 输入 → 336 MB 表**，放大 1139×，峰值 RSS 336 MB，纯头部解码耗时 313 ms。每个组只有 5 个 simple 码（各 1 bit 左右），所以输入极小，而每组仍然要分配 5×256 codes（1280 codes = 5120 B）。

需要的输入量可以算清：meta huffman 精度最小 2，因此要得到 65536 个组索引，至少需要 1024×1024 的图像（huffman 图像 256×256）。这是 65536 组的最小输入，payload 约 295 KB。

### 2.4 颜色缓存放大的乘数

| cc | 每组字节 | 2600 组（合法编码器上限） |
|---|---|---|
| 0 | 11 816 | 30.7 MB |
| 8 | 12 848 | 33.4 MB |
| 11 | 20 016 | 52.0 MB |

实测 `g2600_deep11`（2600 组全深表 + cc=11）= 35 MB 表。颜色缓存把"每组合法开销"抬高 1.7×，且它不需要任何额外输入（4 bit 字段）。

### 2.5 CPU 放大

- 组数路径：295 KB 输入，1 组 0.017 ms，65536 组 313 ms——约 18000×。
- 映射路径（`num_htree_groups_max > 200`，只有 2 个组被使用，但 65536 个索引都要解析）：输入 13.9 MB，耗时 281 ms，**而表只占 23 KB**。这是纯 CPU 放大，与内存无关。
- 不合法树的成本：`VP8LBuildHuffmanTable` 的双遍设计意味着每个被拒绝的树也要完整遍历一次。

### 2.6 内存上限不足时的行为

用 `RLIMIT_AS` 强制分配失败测试 `amp_65536_simple`：

| 限制 | 结果 |
|---|---|
| 400 MB | 成功（表 336 MB 能放下） |
| 250 MB | `WebPDecodeRGBA` 返回 NULL，干净失败 |
| 128 MB | 同上 |
| 64 MB | 同上 |

失败路径干净（无崩溃），但代价是：合法用户如果在 250 MB 限制下解码，这个 295 KB 的输入会把进程逼到 OOM 边界。

### 2.7 与真实编码器对照

用公开编码 API 生成 40 个多样化样本（梯度/噪声/棋盘/分块/纯色 × 8 种尺寸，含 alpha），并统计头部构建：

| 样本 | 尺寸 | 组数 | 表字节 | 耗时 ms |
|---|---|---|---|---|
| real_1024 | 1024² | 5 | 69 360 | 0.13 |
| real_4096 | 4096² | 37 | 509 440 | 1.38 |

真实编码器的组数远低于 `MAX_HUFF_IMAGE_SIZE = 2600`，单树最大 1542 codes，5 棵合计远低于 `kTableSize`。**"合法编码器上限"与"格式允许上限"之间有巨大空隙，这正是攻击面。**

---

## 3. 上限策略

### 3.1 现状小结

| 边界 | 是否有检查 | 是否足够 |
|---|---|---|
| 单棵树合法性（不完整/过度订阅/超长） | 有（`huffman_utils.c` 五处） | 足够 |
| 单棵树大小 ≤ 字母表上界 | 隐含（`kTableSize` 是 5 棵树之和） | 足够 |
| 颜色缓存位范围 | 有（1..11） | 足够 |
| 组数格式上限 | 有（65536，`uint16`） | 格式合法，但**远大于编码器需要** |
| **累计表条目 / 累计表字节** | **无** | **缺失** |
| 映射后未使用组的解析量 | 有循环但无成本上限 | 弱（CPU 放大） |

### 3.2 建议策略

**策略 A（推荐，最小改动）：累计表条目预算**

在 `ReadHuffmanCodesHelper()` 中维护一个 64 位累计量，每构建一棵树就累加其 `size`（单位：`HuffmanCode` 条目），并设：

```
MAX_HUFFMAN_TABLE_CODES = MAX_HUFF_IMAGE_SIZE * (FIXED_TABLE_SIZE + 2704)
                        = 2600 * 5004 = 13 010 400 codes ≈ 49.7 MB
```

三个理由说明这个上界保守又合法：

1. `MAX_HUFF_IMAGE_SIZE = 2600`（`src/dec/common_dec.h:64`）是编码器能产生的最多不同组数——超过它编码器自己就会重新采样。任何合规编码器的组数 ≤ 2600。
2. 单组表条目 ≤ `kTableSize[11] = 5004`（最大的合法单组预算）。
3. 因此 `2600 × 5004` 是所有合规编码器输出都不可能超过的总量；同时把恶意 65536 组样本从 1311 MB（65536 × 5004 × 4）压到 49.7 MB。

实现位置：`ReadHuffmanCodesHelper()` 的逐树循环里（`vp8l_dec.c:556` 附近），在 `size == 0` 检查之后累加并判断；同时可以加一个"单组 5 棵树之和 ≤ kTableSize[cc]"的断言式检查，把"单组畸大"也一并封住。

一个需要留意的交互：`BuildPackedTable()`（`vp8l_dec.c:234`）也从同一个 segment 分配 1024 codes，但它只在 4 个 literal 树的最大码长之和 `< HUFFMAN_PACKED_BITS(6)` 时触发。这样的树最坏每组约 5×384 + 1024 = 2944 codes，仍低于 `kTableSize[0] = 2954`。因此"5 棵树之和 ≤ kTableSize"的检查与 packed 表不冲突：packed 表只会在树很浅时出现，而浅树的表总量远低于上界。修复时不需要把 packed 表计入累计量（它每组固定，且由 `num_htree_groups` 自然受限）。

**策略 B（纵深防御）：组数硬上限**

如果希望更保守，可直接把 `num_htree_groups_max` 的合法上限设为一个与编码器一致的常数（如 2600 或 10000），在映射逻辑之后、构建之前拒绝。代价是需要确认格式规范是否允许更多组——**这是需要在协议层面确认的事，不应单方面收紧**。策略 A 不改变任何合法语义，更安全。

**策略 C（补充）：映射路径的解析成本**

未使用的组仍然被解析（`vp8l_dec.c:528`）。可以在入口用已有的 `(剩余位数) >= num_htree_groups_max * 20` 检查之外，再加"累计解析的树数 ≤ 某个上限"的约束，或直接对 `num_htree_groups_max` 设界。注意：这条检查已经存在（`vp8l_dec.c:504`），它把 65536 组的输入下限抬到 160 KB 左右，所以纯 CPU 路径仍可用 ~160 KB 换取 ~300 ms。

### 3.3 建议保留的上限不变量

- 任何单表的 `size` ∈ {0} ∪ [1, 5004]，且必须通过 `BuildHuffmanTable` 的完整性检查。
- 单组 5 棵树之和 ≤ `kTableSize[cc]`。
- 全流累计表条目 ≤ `MAX_HUFF_IMAGE_SIZE × kTableSize[11]`。
- `color_cache_bits ∈ [1, 11]`（已有）。

---

## 4. 回归测试与"如何证明修复覆盖触发样本"

### 4.1 测试设计

在 `tests/` 下新增一个不依赖外部语料的独立测试，沿用 `0057` 任务已有的 `vp8l_bounds_test.c` 风格（自写 LSB-first 位写入器 + 公开解码 API）：

1. **合法基线（必须成功）**
   - 1×1 trivial 码（cc=0 和 cc=11）。
   - 256×len8 完整绿表。
   - 深完整表（3 个 128 项二级子表）在 5 棵树上，cc=0/cc=11。
   - 2600 组 × 深表 × cc=11（编码器上限组合，实测 35 MB）。
   - 真实编码器生成的 golden 样本（像素 CRC 必须与修复前一致）。

2. **畸形表（必须 `VP8_STATUS_BITSTREAM_ERROR`，且不得分配任何表）**
   - 不完整：2×len2、len1+len3。
   - 过度订阅：3×len1、5×len2。
   - 超长：len16。
   - 码长表膨胀：给出一个码长码自身长度 > 7 的构造（应被 3-bit 字段读法天然限制，测试断言其不可能超过 128）。

3. **总量放大（必须被拒绝）**
   - 最小输入 65536 组、每组 5 个 simple 码（本文 `amp_65536_simple`，295 KB）——修复前 336 MB，修复后 status=3。
   - 阈值两侧：10164 组通过、10165 组被拒（对 trivial 表）。

4. **颜色缓存组合**
   - cc=11 + 深绿表 + 多组，断言"通过修复"且表字节数。

5. **截断与畸形扫描**
   - 对每个样本做逐字节前缀解码：只允许完整长度成功，其余必须干净失败（已在 ASan 下验证）。
   - 在 ASan/UBSan 下全部运行，无 OOB、无泄漏。

### 4.2 如何证明修复覆盖触发样本

这是本任务最关键的方法论问题。建议用**双构建对照**，而不是只看"修复后失败"：

1. **对每个触发样本，同时跑修复前和修复后的二进制**，记录四元组 `(status, 表字节, 峰值 RSS, 耗时)`。
   - 修复前：`status=0`、表字节 = 336 094 304。
   - 修复后：`status=3`、表字节 = 0、RSS 从 336 MB 降到 54 MB。
   - **同一个样本在修复前后状态改变**，才叫"覆盖"。仅"修复后失败"可能是样本本身就坏。
2. **合法样本必须状态不变且像素一致**：40 个真实编码 + 6 个手工合法样本，修复前后 `status` 相同、RGBA CRC 相同（实测 42/42 一致）。
3. **边界样本必须卡在预期位置**：10164 组通过、10165 组拒绝，证明阈值正是 `2600 × 5004` 而不是别的数。
4. **不用 `assert` 判定**：在 `-DNDEBUG` 下也要验证（因为 `VP8LColorCacheLookup` 等路径在 NDEBUG 下会去掉 assert，这也是本任务在 `0057` 中已经踩过的坑）。
5. **ASan/UBSan + 截断扫描**：证明修复的拒绝路径本身没有引入内存错误。

一句话：**"覆盖"的证明是"同一输入在修复前后可观测状态发生了从接受-大内存到拒绝-零内存的转变，同时合法输入在修复前后完全一致"。**

### 4.3 实测证据（本文已完成的对照）

| 类别 | 样本 | 修复前 | 修复后 |
|---|---|---|---|
| 合法深表 | legal_deep_cc11 | status 0, 20016 B | status 0, 20016 B |
| 合法多组 | g2600_deep11 | status 0, 35 388 288 B | status 0, 35 388 288 B |
| 合法真实编码 | 40 个 enc 样本 + 2 个 real | 全部 OK, CRC 一致 | 全部 OK, CRC 一致 |
| 阈值下 | 10164 组 | status 0, 52 226 720 B | status 0, 52 226 720 B |
| 阈值上 | 10165 组 | status 0, 52 226 720 B | **status 3, 0 B** |
| 病理放大 | amp_65536_simple | status 0, 336 094 304 B, RSS 336 MB | **status 3, 0 B, RSS 54 MB** |

---

## 5. 结论

1. **单表层面是安全的**：`BuildHuffmanTable` 对不完整、过度订阅、超长码长都有完备且正确的拒绝路径；码长码表因 3-bit 字段 + `root_bits=7` 硬封顶在 128；`kTableSize` 是与真实构建器一致的合法上界。本任务点名的"码长读取、表大小预估、主表构建、子表构建"这些边界本身没有可被绕过的漏洞。
2. **总量层面缺失边界**：组数（格式允许 65536，编码器只用 ≤2600）、每组预算（`cc` 可从 280 字母表拉到 2328）、以及"未使用组仍被解析"三者叠加，使 295 KB 输入可迫使 336 MB 表分配，或 ~160 KB 输入换取 ~300 ms CPU。
3. **颜色缓存是合法乘数而非独立缺陷**：它只把每组预算从 11816 B 抬到 20016 B（1.7×），单独看完全在 `kTableSize` 之内；与组数相乘才成为放大因子。
4. **推荐修复**：在 `ReadHuffmanCodesHelper()` 增加累计表条目预算 `MAX_HUFF_IMAGE_SIZE × kTableSize[11]`（≈49.7 MB），可选再加单组 5 树之和 ≤ `kTableSize[cc]`。该上界对合规编码器不可达，不改变任何合法语义。
5. **验证方法**：修复前后同一输入的可观测状态对照 + 合法样本像素 CRC 一致 + 阈值两侧取样 + ASan/截断扫描，四者缺一不可。

---

## 附录：复现与工具

文中所有数字来自 `/tmp/vp8lab` 下的最小生成器与测量工具。核心命令：

```sh
# 未修复构建
cmake -B /tmp/b-normal -S . -DWEBP_BUILD_{CWEBP,DWEBP,GIF2WEBP,IMG2WEBP,VWEBP,WEBPINFO,WEBPMUX,ANIM_UTILS,EXTRAS}=OFF
cmake --build /tmp/b-normal -j

# 头部级测量（隔离表构建）
cc -O2 -I<repo> /tmp/vp8lab/measure.c /tmp/b-normal/libwebpdecoder.a -lm -o measure
./measure <sample.webp>          # status / groups / elapsed_ms / peak_rss_kb / table_bytes

# 每次表构建的记录（核实单表上界）
cc -O2 -I<repo> /tmp/vp8lab/sizes2.c /tmp/vp8lab/measure.c \
   /tmp/b-normal/libwebpdecoder.a -lm -Wl,--wrap=VP8LBuildHuffmanTable -o sizes2
```

测量口径说明：

- `peak_rss_kb` 是进程 `ru_maxrss`，包含解码器与测试进程本身的基线（约 1.9 MB），表中数值未扣除基线。
- `table_capacity_bytes` 是 `HuffmanTables` 各 segment 容量之和 × `sizeof(HuffmanCode)=4`，包含未使用的尾部容量（因 segment 追加策略，实测光标值与容量值相差约 1%）。
- 计时为单次头部解码，`RelWithDebInfo`、无 sanitizer；量级用于比较，绝对值受机器负载影响。
