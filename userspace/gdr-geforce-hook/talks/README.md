# 4090 · BAR1 P2P 与 GPUDirect RDMA 讲义

纸面风格。把 P2P 与 GDR 分开讲，再说明为何只改 flag 就能在内核侧适配 GDR，以及用户态为何还要补登记。

| 文件 | 用途 |
|---|---|
| [4090-gdr-ioctl-hook.pptx](4090-gdr-ioctl-hook.pptx) | 讲义。备注是提纲 |
| [build_4090_gdr_lecture.py](build_4090_gdr_lecture.py) | 重新生成 |

## 层次（由下而上）

1. **窗**：BAR1。GPU↔GPU 与网卡 DMA 的数据面。
2. **申报**：`p2pGetCaps` 得到 `PCIE_BAR1`。`NV50_P2P` 与 `NV50_THIRD_PARTY_P2P` 过同一道检查。aikitoria 改的是这类 flag，外加把 BAR1 扩到 32 GB。
3. **登记**：`0x503c`。Tesla 的 libcuda 写，GeForce 不写。`.so` 只补这一层。

## 怎么讲

用演讲者视图。不要把备注念成演说。建议顺序即页序：混淆 → BAR1 → GPU P2P / flag → 为何 GDR 可适配 → 登记与 hook → 验证。

验收：`REGISTER_VIDMEM status=0x0`，且 `gdr_pin_buffer` 不再 `-22`。UMMU 不在本场。

## 重新生成

```bash
python3 userspace/gdr-geforce-hook/talks/build_4090_gdr_lecture.py
```

标题字体按「宋体」、正文「微软雅黑」。若方框，在演示软件里换成机器上的对应 CJK 字体。

## 不要提倡

- 为 GDR 再改 4090 的 NVIDIA 内核
- 全局 `LD_PRELOAD`
- 把 Harry 的 libcuda 一字节当成 peermem 的完整方案
