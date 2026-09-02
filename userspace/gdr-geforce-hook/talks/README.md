# 4090 打通 GDR 讲座 PPT

面向内核背景不多的基础程序员。45–60 分钟。

| 文件 | 用途 |
|---|---|
| [4090-gdr-ioctl-hook.pptx](4090-gdr-ioctl-hook.pptx) | 讲稿。每页备注栏是口播稿，请用**演讲者视图** |
| [build_4090_gdr_lecture.py](build_4090_gdr_lecture.py) | 重新生成 PPT |

## 怎么讲

1. PowerPoint / LibreOffice 打开 pptx，切到**演讲者视图**（备注在下半屏）。
2. 不要只念正文。正文是给听众看的骨架，细节在备注里。
3. 建议节奏：开场+比喻 10 分钟 → Harry 5090 8 分钟 → 弯路与借鉴 6 分钟 → ioctl / VMM / 登记本 20 分钟 → 对照、怎么跑、Q&A 12 分钟。
4. 验收标准只讲到：`gdr_pin_buffer` 不再 `-22`，日志里有 `REGISTER_VIDMEM status=0x0`。UMMU / SVA / MATT 是下一场，本场只留一页边界。

## 重新生成

```bash
python3 -m pip install python-pptx
python3 userspace/gdr-geforce-hook/talks/build_4090_gdr_lecture.py
```

字体按 Windows 讲者习惯写成「微软雅黑」+ Consolas。若中文方框：装微软雅黑，或在演示软件里把主题字体换成本机 CJK 字体。

## 不要在讲座里提倡的

- 为 GDR 再改 4090 的 NVIDIA 内核（BAR1 P2P 用 aikitoria 即可）
- 全局 `export LD_PRELOAD` 或写 `/etc/ld.so.preload`
- 把 Harry 的 libcuda 一字节补丁当成 GDRCopy / peermem 的完整方案
