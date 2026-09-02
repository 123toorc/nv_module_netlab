#!/usr/bin/env python3
"""4090 GDR 讲义。纸面古典风格。重写请直接改本文件后重新生成。"""

from __future__ import annotations

from pathlib import Path

from lxml import etree
from pptx import Presentation
from pptx.dml.color import RGBColor
from pptx.enum.shapes import MSO_SHAPE
from pptx.enum.text import MSO_ANCHOR, PP_ALIGN
from pptx.oxml.ns import qn
from pptx.util import Inches, Pt

OUT = Path(__file__).resolve().parent / "4090-gdr-ioctl-hook.pptx"
SW, SH = Inches(13.333), Inches(7.5)

FONT_TITLE = "宋体"
FONT_BODY = "微软雅黑"
FONT_MONO = "Consolas"

PAPER = RGBColor(0xF3, 0xED, 0xE0)
INK = RGBColor(0x2A, 0x24, 0x1C)
FAINT = RGBColor(0x5A, 0x50, 0x44)
RULE = RGBColor(0x8A, 0x6F, 0x4E)
BOX = RGBColor(0xEA, 0xE3, 0xD4)
BOX2 = RGBColor(0xDF, 0xD6, 0xC6)
RED = RGBColor(0x7A, 0x24, 0x1C)
CREAM = RGBColor(0xF7, 0xF2, 0xE8)
CODE_BG = RGBColor(0x2A, 0x24, 0x1C)
CODE_FG = RGBColor(0xF0, 0xEA, 0xDC)
CODE_DIM = RGBColor(0xB8, 0xAE, 0x9A)

TOTAL = [0]


def style_run(run, size, color, bold=False, mono=False, title=False):
    run.font.size = Pt(size)
    run.font.bold = bold
    run.font.color.rgb = color
    latin = FONT_MONO if mono else (FONT_TITLE if title else FONT_BODY)
    ea = FONT_TITLE if title else FONT_BODY
    run.font.name = latin
    rPr = run._r.get_or_add_rPr()
    for tag, name in (("a:latin", latin), ("a:ea", ea), ("a:cs", latin)):
        el = rPr.find(qn(tag))
        if el is None:
            el = etree.SubElement(rPr, qn(tag))
        el.set("typeface", name)


def set_bg(slide, color=PAPER):
    fill = slide.background.fill
    fill.solid()
    fill.fore_color.rgb = color


def notes(slide, text):
    tf = slide.notes_slide.notes_text_frame
    tf.clear()
    p = tf.paragraphs[0]
    run = p.add_run()
    run.text = text.strip() + "\n"
    style_run(run, 13, RGBColor(0x22, 0x22, 0x22))


def rect(slide, l, t, w, h, fill, line=None, lw=1.0):
    shp = slide.shapes.add_shape(
        MSO_SHAPE.RECTANGLE, Inches(l), Inches(t), Inches(w), Inches(h)
    )
    shp.fill.solid()
    shp.fill.fore_color.rgb = fill
    if line is None:
        shp.line.fill.background()
    else:
        shp.line.color.rgb = line
        shp.line.width = Pt(lw)
    return shp


def tb(slide, l, t, w, h, lines, size=15, color=INK, bold=False, align="left",
       mono=False, title=False, anchor=MSO_ANCHOR.TOP, spacing=1.15):
    tx = slide.shapes.add_textbox(Inches(l), Inches(t), Inches(w), Inches(h))
    tf = tx.text_frame
    tf.word_wrap = True
    tf.auto_size = None
    tf.margin_left = Inches(0.04)
    tf.margin_right = Inches(0.04)
    tf.margin_top = Inches(0.02)
    tf.margin_bottom = Inches(0.02)
    try:
        tf._txBody.bodyPr.set("anchor", {
            MSO_ANCHOR.TOP: "t",
            MSO_ANCHOR.MIDDLE: "ctr",
            MSO_ANCHOR.BOTTOM: "b",
        }.get(anchor, "t"))
    except Exception:
        pass
    if isinstance(lines, str):
        lines = lines.split("\n")
    amap = {"left": PP_ALIGN.LEFT, "center": PP_ALIGN.CENTER, "right": PP_ALIGN.RIGHT}
    for i, item in enumerate(lines):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = amap[align]
        p.space_after = Pt(1)
        p.line_spacing = spacing
        if isinstance(item, tuple):
            text, sz, col, bd = item[0], item[1], item[2], item[3]
            it_mono = item[4] if len(item) > 4 else mono
            it_title = item[5] if len(item) > 5 else title
        else:
            text, sz, col, bd, it_mono, it_title = item, size, color, bold, mono, title
        run = p.add_run()
        run.text = text
        style_run(run, sz, col, bold=bd, mono=it_mono, title=it_title)
    return tx


def footer(slide, page, kicker=""):
    rect(slide, 0.55, 7.18, 12.25, 0.012, RULE)
    tb(slide, 0.55, 7.22, 8.4, 0.22, kicker or "4090 · BAR1 P2P 与 GPUDirect RDMA",
       11, FAINT, title=True)
    tb(slide, 10.4, 7.22, 2.4, 0.22, f"{page}  /  {TOTAL[0]}",
       11, FAINT, align="right")


def head(slide, sect, title, page):
    set_bg(slide)
    rect(slide, 0.55, 0.28, 0.08, 0.28, RED)
    tb(slide, 0.75, 0.26, 11.8, 0.30, sect, 12, RED, True, title=True)
    tb(slide, 0.55, 0.56, 12.2, 0.42, title, 22, INK, True, title=True)
    rect(slide, 0.55, 1.02, 2.1, 0.018, RULE)
    footer(slide, page, sect)


def new(prs):
    return prs.slides.add_slide(prs.slide_layouts[6])


def frame(slide, l, t, w, h, fill=BOX):
    return rect(slide, l, t, w, h, fill, RULE, 0.9)


def build():
    prs = Presentation()
    prs.slide_width = SW
    prs.slide_height = SH
    slides = []

    def add():
        s = new(prs)
        slides.append(s)
        return s

    def s_title():
        s = add()
        set_bg(s)
        rect(s, 0.55, 0.55, 12.25, 0.018, RULE)
        tb(s, 0.55, 0.85, 12.2, 0.32, "内部讲义  ·  RTX 4090 D  ·  驱动 575.64.05", 13, RED, True, title=True)
        tb(s, 0.55, 1.55, 12.2, 1.15, "BAR1 P2P 与 GPUDirect RDMA", 34, INK, True, title=True)
        tb(s, 0.55, 2.75, 12.2, 0.55, "连通性、策略位，以及用户态为何还要补登记", 18, FAINT, title=True)
        rect(s, 0.55, 3.50, 2.1, 0.018, RULE)
        tb(s, 0.55, 3.75, 12.2, 2.4,
           "本稿把两件常被混为一谈的事分开：GPU 与 GPU 的 P2P，以及网卡对 GPU 显存的 RDMA（GDR）。\n"
           "前者在本机已由 aikitoria 575.64.05-p2p 打开——改的是申报用的 flag，不是另做一条 DMA。\n"
           "后者与前者共用 BAR1 窗口与同一道内核连通性检查，故内核侧可以直接适配；\n"
           "GeForce 的 libcuda 仍不写第三方登记，故 pin 还须用户态补手续。",
           15, INK, spacing=1.25)
        tb(s, 0.55, 6.45, 12.2, 0.55,
           "听众：写过 C、用过动态库即可。不要求写过内核。备注栏是提纲，不是讲演稿。",
           13, FAINT)
        footer(s, 1, "题")
        notes(s, """
先读标题下的四句。不要把本场讲成「我们发明了 GDR」。
本场顺序：先分清 P2P 与 GDR，再讲 BAR1，再讲只改 flag 为何够，再讲 GDR 为何能直接接上，最后才是 ioctl 补登记。
""")

    def s_confusion():
        s = add()
        head(s, "一　混淆", "两件事，不是一个功能的两个名字", len(slides))
        frame(s, 0.55, 1.25, 6.00, 5.55)
        tb(s, 0.75, 1.40, 5.6, 0.36, "常被说成一回事", 14, RED, True, title=True)
        tb(s, 0.75, 1.90, 5.6, 4.6,
           "「消费卡不支持 P2P，所以也不支持 GDR。」\n"
           "「P2P 已经通了，GDR 就应当立刻通。」\n"
           "「GDR 要另写一套内核 DMA。」\n"
           "「改内核能力位，CUDA 就会承认 GDR。」\n\n"
           "四句都不准确。前两句把「窗口」和「登记」混在一起；\n"
           "后两句把「连通性」和「用户态政策」混在一起。",
           15, INK, spacing=1.22)
        frame(s, 6.75, 1.25, 6.00, 5.55, BOX2)
        tb(s, 6.95, 1.40, 5.6, 0.36, "本稿的分法", 14, RED, True, title=True)
        tb(s, 6.95, 1.90, 5.6, 4.6,
           "P2P：PCIe 对端（通常是另一块 GPU）经 BAR1 访问本卡显存。\n\n"
           "GDR：PCIe 对端换成网卡。内核入口是 nvidia_p2p_get_pages。\n\n"
           "二者共用同一扇 BAR1 窗，也共用 p2pGetCaps 那道连通性检查。\n\n"
           "差在手续：GPU P2P 由 CUDA/UVM 自己建映射；\n"
           "GDR 要第三方先在 RM 里登记 VA，GeForce 的 libcuda 不做这一步。",
           15, INK, spacing=1.22)
        notes(s, """
这一页只立定义。后面所有「为什么 P2P 通了 GDR 还能失败」都回到右边四段。
""")

    def s_terms():
        s = add()
        head(s, "一　混淆", "用语。先定名，再谈实现", len(slides))
        rows = [
            ("BAR1", "GPU 把一段显存窗口映射到 PCIe 配置空间。对端 DMA 打的是这扇窗，不是「再拷一份到主机」。"),
            ("GPU P2P", "对端是另一块 GPU。cudaMemcpyPeer / UVM 在对端页表里填上本卡 BAR1 地址。"),
            ("GDR", "GPUDirect RDMA。对端是网卡。GDRCopy、nvidia-peermem 走 nvidia_p2p_get_pages。"),
            ("dma-buf", "较新的导出路径。NCCL 常用。Harry 在 5090 上打开的是这一扇，不是旧 peermem。"),
            ("登记", "RM 对象 NV50_THIRD_PARTY_P2P（0x503c）里的 VA 记录。get_pages 按地址查这本书。"),
        ]
        y = 1.22
        for a, b in rows:
            frame(s, 0.55, y, 12.25, 1.08)
            tb(s, 0.75, y, 2.15, 1.08, a, 15, RED, True, mono=True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 3.00, y + 0.12, 9.55, 0.86, b, 15, INK, anchor=MSO_ANCHOR.MIDDLE)
            y += 1.14
        notes(s, """
BAR1、P2P、GDR、dma-buf、登记，五个词本场反复用。不要用「直通」「零拷贝」代替它们。
""")

    def s_table():
        s = add()
        head(s, "一　混淆", "同一扇窗，两套手续", len(slides))
        cols = [0.55, 2.55, 5.85, 9.15]
        widths = [2.0, 3.3, 3.3, 3.65]
        headers = ["", "GPU ↔ GPU P2P", "GDR（peermem）", "dma-buf / NCCL"]
        data = [
            ["对端", "另一块 GPU", "网卡", "网卡（新路径）"],
            ["物理窗", "BAR1", "BAR1", "dma-buf 导出"],
            ["内核入口", "UVM / NV50_P2P", "nvidia_p2p_get_pages", "dma-buf fd"],
            ["谁建映射", "CUDA / UVM", "须先有 0x503c 登记", "libcuda 导出"],
            ["4090 现状", "flag 打开后可用", "内核已适配，缺登记", "本稿不走这条"],
        ]
        y = 1.22
        frame(s, 0.55, y, 12.25, 0.48, BOX2)
        for i, h in enumerate(headers):
            tb(s, cols[i], y, widths[i], 0.48, h, 13, RED, True, title=True,
               align="center" if i else "left", anchor=MSO_ANCHOR.MIDDLE)
        y = 1.70
        for r, row in enumerate(data):
            bg = BOX if r % 2 == 0 else CREAM
            rect(s, 0.55, y, 12.25, 0.95, bg, RULE, 0.6)
            for i, cell in enumerate(row):
                tb(s, cols[i], y, widths[i], 0.95, cell, 14, INK,
                   bold=(i == 0), align="center" if i else "left",
                   anchor=MSO_ANCHOR.MIDDLE)
            y += 0.95
        notes(s, """
指中间两列：物理窗都是 BAR1，所以 P2P 的 flag 对 GDR 有效。差在「谁建映射」。
右列点一下即可：Harry 的文章走的是它，不要和 peermem 并成一句。
""")

    def s_pcie():
        s = add()
        head(s, "二　BAR1", "PCIe 只需三个概念", len(slides))
        items = [
            ("功能（Function）",
             "每个设备在总线上是一个请求者，也可以是一个被访问的目标。GPU 与网卡都是。"),
            ("BAR（Base Address Register）",
             "设备向主机申报的一段可寻址窗口。主机或其他设备把事务打到这段地址，理论上就打到该设备内部。"),
            ("DMA",
             "设备自己当请求者去读/写别人的地址。网卡发报文时，读的是它认为合法的 DMA 地址，不必经过 CPU 拷贝。"),
        ]
        y = 1.22
        for t, b in items:
            frame(s, 0.55, y, 12.25, 1.55)
            tb(s, 0.80, y + 0.16, 11.8, 0.40, t, 16, RED, True, title=True)
            tb(s, 0.80, y + 0.62, 11.8, 0.75, b, 15, INK)
            y += 1.68
        tb(s, 0.55, 6.35, 12.2, 0.65,
           "因此「P2P 通了」的硬件含义只有一句：对端发出的 PCIe 事务，能够打中本卡申报出来的那扇窗。",
           15, FAINT)
        notes(s, """
不要展开 TLP、ATS、IOMMU 页表。三个词够用。
DMA 强调：CPU 不参与数据面。后面 GDR 省的就是这一跳。
""")

    def s_bar01():
        s = add()
        head(s, "二　BAR1", "NVIDIA GPU 常见的两扇窗", len(slides))
        frame(s, 0.55, 1.25, 6.00, 5.55)
        tb(s, 0.80, 1.45, 5.5, 0.40, "BAR0", 18, RED, True, title=True)
        tb(s, 0.80, 2.00, 5.5, 4.4,
           "寄存器与控制面。\n"
           "驱动用它跟 GPU 说话：门铃、MMIO、配置。\n\n"
           "对 GDR / P2P 的数据面不重要。\n"
           "本稿不再展开。",
           15, INK, spacing=1.25)
        frame(s, 6.75, 1.25, 6.00, 5.55, BOX2)
        tb(s, 7.00, 1.45, 5.5, 0.40, "BAR1", 18, RED, True, title=True)
        tb(s, 7.00, 2.00, 5.5, 4.4,
           "Framebuffer 的窗口。\n"
           "一段 PCIe 地址，背后是 GPU 显存的一部分或全部。\n\n"
           "官方默认往往只有数百 MB。\n"
           "本机扩到 32 GB，使窗口能盖住工作集。\n\n"
           "GPU P2P 与 GDR 的数据都走这扇窗。",
           15, INK, spacing=1.25)
        notes(s, """
可画：左边小窗写 MMIO，右边大窗写 FB。强调 BAR1 不是「第二份内存」，是同一份显存的外部可见入口。
""")

    def s_window():
        s = add()
        head(s, "二　BAR1", "窗口与拷贝不是一回事", len(slides))
        # simple diagram
        frame(s, 0.55, 1.30, 3.6, 2.15)
        tb(s, 0.55, 1.30, 3.6, 2.15, "GPU 显存\n（物理 FB）", 16, INK, True, align="center",
           anchor=MSO_ANCHOR.MIDDLE, title=True)
        frame(s, 5.00, 1.30, 3.6, 2.15, BOX2)
        tb(s, 5.00, 1.30, 3.6, 2.15, "BAR1 窗口\n（PCIe 上可见）", 16, INK, True, align="center",
           anchor=MSO_ANCHOR.MIDDLE, title=True)
        frame(s, 9.40, 1.30, 3.4, 2.15)
        tb(s, 9.40, 1.30, 3.4, 2.15, "对端\nGPU 或 NIC", 16, INK, True, align="center",
           anchor=MSO_ANCHOR.MIDDLE, title=True)
        tb(s, 4.15, 2.00, 0.85, 0.50, "映射", 13, FAINT, align="center", title=True)
        tb(s, 8.60, 2.00, 0.80, 0.50, "DMA", 13, FAINT, align="center", title=True)
        tb(s, 0.55, 3.70, 12.25, 3.10,
           "对端并不先把数据拷到主机内存。它把事务发到 BAR1 这一段 PCIe 地址，\n"
           "GPU 的内存子系统把这段地址翻译回 FB 页。\n\n"
           "所以：打开 P2P，本质是允许并完成这套「申报窗口 + 对端映射」。\n"
           "不是在内核里新写一个拷贝引擎。驱动里 BAR1 P2P 的实现（页表、SYS_NONCOH 孔径、\n"
           "UVM 的 PCIE_BAR1 连接类型）本来就在；GeForce 默认不选用它。",
           15, INK, spacing=1.22)
        notes(s, """
「映射」与「拷贝」一定要分开。听众里很多人把 P2P 想成驱动帮忙 memcpy。
引用：610 一代已经带齐原生 BAR1 P2P，消费卡只是不被选中。aikitoria 做的是选中它。
""")

    def s_gpup2p():
        s = add()
        head(s, "三　GPU P2P", "GPU 对 GPU：对端页表填上本卡 BAR1", len(slides))
        steps = [
            ("1", "能力查询", "cudaDeviceCanAccessPeer / nvidia-smi topo -p2p。问的是 p2pGetCaps 的结果。"),
            ("2", "建立 peer", "cudaDeviceEnablePeerAccess。UVM 在本卡与对端之间建一条 PCIE_BAR1 连接。"),
            ("3", "映射", "对端 GPU 的 MMU 里，出现指向本卡 BAR1 的 PTE。之后 memcpyPeer 是设备自己的 DMA。"),
            ("4", "数据面", "不经主机 bounce。窗口不够大时，小块临时映射仍可能成功，大块持久映射会失败。"),
        ]
        y = 1.22
        for n, t, b in steps:
            frame(s, 0.55, y, 12.25, 1.28)
            tb(s, 0.75, y, 0.55, 1.28, n, 20, RED, True, title=True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 1.45, y + 0.14, 2.6, 1.00, t, 16, INK, True, title=True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 4.15, y + 0.18, 8.4, 0.95, b, 15, INK, anchor=MSO_ANCHOR.MIDDLE)
            y += 1.36
        notes(s, """
四步按时间说。第 4 步预告下一页 BAR1 尺寸：topo 报 OK 不等于 NCCL 那种持久映射能放下。
""")

    def s_conn():
        s = add()
        head(s, "三　GPU P2P", "内核如何描述「通了」", len(slides))
        tb(s, 0.55, 1.18, 12.2, 0.55,
           "p2p_caps.h 里的连通性不是营销词，是 RM 的枚举。4090 走最后一项。",
           14, FAINT)
        rows = [
            ("NVLINK / C2C", "数据中心卡或特殊封装。4090 无 NVLink，不走这里。"),
            ("PCIE_PROPRIETARY", "较旧的 PCIe P2P 邮箱路径。"),
            ("PCIE_BAR1", "经 BAR1 窗做 peer 映射。本机 GPU↔GPU 通的就是这一项。"),
        ]
        y = 1.80
        for a, b in rows:
            frame(s, 0.55, y, 12.25, 1.15)
            tb(s, 0.80, y, 3.6, 1.15, a, 16, RED, True, mono=True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 4.50, y, 8.05, 1.15, b, 15, INK, anchor=MSO_ANCHOR.MIDDLE)
            y += 1.25
        tb(s, 0.55, 5.70, 12.2, 1.20,
           "forceP2PType、pcieP2PType 决定走哪条分支。\n"
           "aikitoria 把 PCIe P2P 类型落到 BAR1，并把读写能力位置上。",
           15, INK)
        notes(s, """
打开 p2p_caps.c 的枚举口播即可，不必投影源码全文。
""")

    def s_shared_gate():
        s = add()
        head(s, "三　GPU P2P", "两类对象，过同一道检查", len(slides))
        frame(s, 0.55, 1.22, 12.25, 2.55, CODE_BG)
        tb(s, 0.80, 1.40, 11.8, 2.20, [
            ("// p2pGetCaps()  p2p_caps.c", 13, CODE_DIM, False, True),
            ("// The classes like NV50_P2P, NV50_THIRD_PARTY_P2P", 14, CODE_FG, False, True),
            ("// depend on direct P2P connectivity, hence the check.", 14, CODE_FG, False, True),
            ("connectivity ∈ { PCIE_BAR1, PCIE_PROPRIETARY, NVLINK, C2C }", 14, CODE_FG, False, True),
            ("otherwise  ->  NV_ERR_NOT_SUPPORTED", 14, CODE_FG, False, True),
        ])
        tb(s, 0.55, 4.00, 12.2, 2.80,
           "NV50_P2P：GPU 与 GPU 的 peer 对象。\n"
           "NV50_THIRD_PARTY_P2P（0x503c）：给第三方（网卡驱动、GDRCopy）用的登记对象。\n\n"
           "二者都要求「直接 P2P 连通性」为真。把 BAR1 P2P 的 flag 打开以后，\n"
           "不是只放行了 cudaMemcpyPeer；连 0x503c 的构造许可也一并放开。\n"
           "这就是「GDR 在内核侧可以直接适配」的依据，写在官方注释里。",
           15, INK, spacing=1.22)
        notes(s, """
本场最重要的一页之一。慢慢念注释。
「直接适配」= 不需要再为 GDR 打第二套内核补丁。TPP 类已经依赖同一连通性。
尚未适配的是用户态写不写登记，下一章再讲。
""")

    def s_policy():
        s = add()
        head(s, "三　GPU P2P", "GeForce 默认关掉的是申报", len(slides))
        frame(s, 0.55, 1.22, 12.25, 2.35)
        tb(s, 0.80, 1.40, 11.8, 2.05,
           "芯片有 BAR1，驱动里也有完整的 BAR1 P2P 实现（页表、孔径、UVM 连接类型）。\n"
           "产品策略不把这条路径选给消费卡：能力查询返回不支持，或 PCIe P2P 类型不落到 BAR1。\n"
           "chipset allowlist、p2pOverride、SKU 相关的 feature disablement，都属于这一层。",
           15, INK, spacing=1.25)
        frame(s, 0.55, 3.75, 6.00, 3.05)
        tb(s, 0.75, 3.92, 5.6, 0.36, "不是", 14, RED, True, title=True)
        tb(s, 0.75, 4.40, 5.6, 2.15,
           "4090 的 BAR 不能被对端寻址。\n"
           "缺少一套「消费卡专用」的 DMA IP。\n"
           "必须重写 nvidia_p2p_get_pages。",
           15, INK, spacing=1.25)
        frame(s, 6.75, 3.75, 6.05, 3.05, BOX2)
        tb(s, 6.95, 3.92, 5.65, 0.36, "是", 14, RED, True, title=True)
        tb(s, 6.95, 4.40, 5.65, 2.15,
           "查询路径上把能力报成「无」。\n"
           "不选用已经存在的 PCIE_BAR1 分支。\n"
           "BAR1 默认窗口过小，盖不住整卡 FB。",
           15, INK, spacing=1.25)
        notes(s, """
和 Harry 后文呼应：闸在策略。这里先只谈内核申报，用户态 116 留给后文。
""")

    def s_flags():
        s = add()
        head(s, "三　GPU P2P", "所改者，是 flag，不是协议", len(slides))
        rows = [
            ("p2pOverride / ForceP2P",
             "绕过 chipset allowlist，强制申报 P2P 读、写能力。常见写成 0x11。"),
            ("pcieP2PType = BAR1",
             "让 PCIe P2P 落到 BAR1 分支。kbusIsPcieBar1P2PMappingSupported 检查的就是它。"),
            ("forceP2PType = PCIEP2P",
             "不要被 NVLink/C2C 探测支走。4090 本来也没有 NVLink。"),
            ("BAR1 扩到 32 GB",
             "窗口盖住工作集。默认数百 MB 时，topo 仍可能报 OK，大块持久映射放不下。"),
        ]
        y = 1.20
        for a, b in rows:
            frame(s, 0.55, y, 12.25, 1.30)
            tb(s, 0.80, y + 0.18, 4.3, 0.95, a, 15, RED, True, mono=True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 5.20, y + 0.18, 7.35, 0.95, b, 15, INK, anchor=MSO_ANCHOR.MIDDLE)
            y += 1.38
        notes(s, """
对应 aikitoria 575.64.05-p2p 一类工作：force-enable，不是手写一套 BAR1。
本机不要再为「开 GDR」继续改 NVIDIA 内核。这些 flag 已经把连通性放到 PCIE_BAR1。
BAR1 尺寸是窗口几何，不是能力 bit，但同属「让现有实现能用」而不是新协议。
""")

    def s_enough():
        s = add()
        head(s, "三　GPU P2P", "为何这些 flag 足够让 P2P 工作", len(slides))
        tb(s, 0.55, 1.20, 12.2, 5.55,
           "驱动在 Ada / Hopper 一代已经带了原生 BAR1 P2P：\n"
           "外部 PTE、SYS_NONCOH 孔径、nv_gpu_ops 里的 BAR1 DMA 地址、UVM 的 PCIE_BAR1。\n\n"
           "消费卡上这条链默认不被选中。把申报位和类型位拨正，查询返回 OK，\n"
           "UVM 就会走现成实现，cudaMemcpyPeer 成为设备 DMA，而不是主机中转。\n\n"
           "没有增加新的 PCIe 消息类型，也没有在 4090 上补一块「原来没有的 P2P 硬件」。\n"
           "验证标准是对端访问经 BAR1 命中 FB，而不是「内核里多了一份补丁行数」。\n\n"
           "这也解释了为何社区补丁可以很小：改的是选择，不是算法。",
           16, INK, spacing=1.28)
        notes(s, """
QuixiAI 对 aikitoria 谱系的概括可以口播：surgical force-enables。
若有人问 3090 NVLink：那是另一条连通性，4090 只有 BAR1。
""")

    def s_barsize():
        s = add()
        head(s, "三　GPU P2P", "窗口大小是几何问题", len(slides))
        frame(s, 0.55, 1.22, 6.00, 5.55)
        tb(s, 0.80, 1.42, 5.55, 0.40, "小 BAR1（默认）", 16, RED, True, title=True)
        tb(s, 0.80, 2.00, 5.55, 4.4,
           "只有一小段 FB 同时出现在 PCIe 上。\n"
           "短时、小块的 peer 拷贝可以靠滑动窗口完成，\n"
           "故 nvidia-smi topo -p2p 与简单 memcpyPeer 仍可能成功。\n\n"
           "持久、大块的映射（NCCL 集合通信一类）\n"
           "放不进窗口，表现为挂起或回退。",
           15, INK, spacing=1.25)
        frame(s, 6.75, 1.22, 6.05, 5.55, BOX2)
        tb(s, 7.00, 1.42, 5.6, 0.40, "大 BAR1（本机 32 GB）", 16, RED, True, title=True)
        tb(s, 7.00, 2.00, 5.6, 4.4,
           "窗口盖住整卡或工作集。\n"
           "对端可以同时映射大段 FB。\n\n"
           "GDR pin 一段连续 VA 时，对应的 BAR1 物理页\n"
           "也必须落在这扇窗里。\n\n"
           "故扩 BAR1 是 P2P 与 GDR 的共同前提，\n"
           "不是「只给 GPU 互访用的附加项」。",
           15, INK, spacing=1.25)
        notes(s, """
把「topo OK 但大块不行」说清楚，避免听众以为 flag 之外无事。
GDR 的 64K 对齐页同样要落在 BAR1 覆盖范围内。
""")

    def s_machine():
        s = add()
        head(s, "三　GPU P2P", "本机已成立的事实", len(slides))
        rows = [
            ("卡与驱动", "RTX 4090 D，NVIDIA 575.64.05，CUDA 12.9，AArch64"),
            ("内核", "aikitoria 575.64.05-p2p。GPU↔GPU BAR1 P2P 已通。"),
            ("BAR1", "32 GB。不要再为 GDR 改这份 NVIDIA 内核。"),
            ("尚未成立", "GDRCopy gdr_pin_buffer → nvidia_p2p_get_pages 仍曾返回 -22。"),
        ]
        y = 1.22
        for a, b in rows:
            frame(s, 0.55, y, 12.25, 1.28)
            tb(s, 0.80, y, 2.6, 1.28, a, 15, RED, True, title=True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 3.55, y, 9.00, 1.28, b, 15, INK, anchor=MSO_ANCHOR.MIDDLE)
            y += 1.36
        notes(s, """
「已通 / 未通」画一条线。下面进入 GDR：为何内核已适配，应用仍失败。
""")

    def s_gdr_def():
        s = add()
        head(s, "四　GDR", "对端换成网卡，窗还是那一扇", len(slides))
        frame(s, 0.55, 1.22, 12.25, 2.20)
        tb(s, 0.80, 1.40, 11.8, 1.85,
           "GPUDirect RDMA：网卡作为 PCIe 请求者，按 GPU 的地址去 DMA 读或写显存。\n"
           "省掉的是主机 bounce：不再 GPU→CPU 内存→NIC。\n"
           "GDRCopy 的 gdr_pin_buffer，问的就是：这段 CUDA VA，能否变成网卡可用的页表。",
           15, INK, spacing=1.25)
        # two columns
        frame(s, 0.55, 3.60, 6.00, 3.20)
        tb(s, 0.75, 3.78, 5.6, 0.36, "P2P 时", 14, RED, True, title=True)
        tb(s, 0.75, 4.25, 5.6, 2.30,
           "对端 GPU 自己有 MMU。\n"
           "CUDA/UVM 替它填 PTE。\n"
           "应用不必向 RM「登记」这块 VA。",
           15, INK, spacing=1.25)
        frame(s, 6.75, 3.60, 6.05, 3.20, BOX2)
        tb(s, 6.95, 3.78, 5.65, 0.36, "GDR 时", 14, RED, True, title=True)
        tb(s, 6.95, 4.25, 5.65, 2.30,
           "网卡没有 GPU MMU。\n"
           "它向 NVIDIA 驱动要一张物理页表。\n"
           "驱动只在登记本上有这条 VA 时才给。",
           15, INK, spacing=1.25)
        notes(s, """
「窗相同、手续不同」是全场第二句口号级的话，但用陈述句说，不要写成标语。
""")

    def s_get_pages():
        s = add()
        head(s, "四　GDR", "nvidia_p2p_get_pages 在要什么", len(slides))
        frame(s, 0.55, 1.22, 12.25, 2.70, CODE_BG)
        tb(s, 0.80, 1.40, 11.8, 2.35, [
            ("int nvidia_p2p_get_pages(token, va_space, va, length, &page_table, ...);", 14, CODE_FG, False, True),
            ("/* 只要 pinned、在 GPU 上的内存，如 cudaMalloc */", 13, CODE_DIM, False, True),
            ("/* va、length 须 64KB 对齐 */", 13, CODE_DIM, False, True),
            ("/* 成功：page_table->pages[i].physical_address 可供第三方 DMA */", 13, CODE_DIM, False, True),
        ])
        tb(s, 0.55, 4.15, 12.2, 2.65,
           "随后 nvidia_p2p_dma_map_pages(peer_pci_dev, page_table, &dma_mapping)\n"
           "把这些物理页变成该网卡可用的 DMA 地址。\n\n"
           "GDRCopy、nvidia-peermem 都走这组导出符号。它们不实现 GPU 页表，只向 NVIDIA 驱动查询。\n"
           "查询失败时 errno 为 EINVAL，RM 状态常见 0x57（OBJECT_NOT_FOUND）。",
           15, INK, spacing=1.22)
        notes(s, """
对着 nv-p2p.h 的注释讲。physical_address 在 BAR1 路径上就是窗口里的地址。
0x57 翻译：不是网卡坏了，是这本书上没有这个 VA。
""")

    def s_adapt():
        s = add()
        head(s, "四　GDR", "为何 P2P 一通，GDR 在内核侧即可适配", len(slides))
        rows = [
            "连通性已经是 PCIE_BAR1。p2pGetCaps 允许构造 NV50_THIRD_PARTY_P2P。",
            "get_pages 的实现本来就会按登记项去建 BAR1 上的页表，并不另要一套「GDR 专用引擎」。",
            "网卡与对端 GPU 都是 PCIe 请求者。对 RM 而言，差在调用者是 UVM 还是第三方模块。",
            "BAR1 已经够大。pin 出来的页落在同一扇窗里，dma_map_pages 才能交给 NIC。",
        ]
        y = 1.22
        for i, line in enumerate(rows, 1):
            frame(s, 0.55, y, 12.25, 1.18)
            tb(s, 0.75, y, 0.55, 1.18, str(i), 18, RED, True, title=True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 1.45, y, 11.1, 1.18, line, 16, INK, anchor=MSO_ANCHOR.MIDDLE)
            y += 1.28
        notes(s, """
四条就是「直接适配」的定义。不要说成「P2P 通了 GDR 就一定通」——缺登记仍会 0x57。
""")

    def s_why_fail():
        s = add()
        head(s, "四　GDR", "应用层仍然失败的原因", len(slides))
        tb(s, 0.55, 1.20, 12.2, 1.00,
           "get_pages 在 RM 内按 VA 查找 CliGetThirdPartyP2PVidmemInfoFromAddress。\n"
           "找不到，返回 NV_ERR_OBJECT_NOT_FOUND（0x57），映射到用户态 -EINVAL（-22）。",
           15, INK)
        frame(s, 0.55, 2.40, 12.25, 2.15, CODE_BG)
        tb(s, 0.80, 2.55, 11.8, 1.85, [
            ("// p2p.c", 13, CODE_DIM, False, True),
            ("// GeForce / consumer CUDA never sends NV503C", 14, CODE_FG, False, True),
            ("// REGISTER_VA_SPACE / REGISTER_VIDMEM.", 14, CODE_FG, False, True),
            ("// nvidia_p2p_get_pages() then fails with 0x57", 14, CODE_FG, False, True),
            ("// even though a ThirdPartyP2P object may exist.", 14, CODE_FG, False, True),
        ])
        tb(s, 0.55, 4.75, 12.2, 2.05,
           "Tesla / 数据中心卡的 libcuda：自己 Alloc 0x503c，再登记 VA space 与 vidmem。\n"
           "GeForce 的 libcuda：不发这三步。内核再允许构造 TPP，本上仍是空的。\n"
           "故：P2P flag 解决连通性；登记是另一层，不能靠再翻一个内核 bit 补上。",
           15, INK, spacing=1.22)
        notes(s, """
这是「只改 flag 不够用在应用 pin 上」的精确说法。flag 对内核适配是够的；对 libcuda 写本不够。
本树 p2p.c 里还有一段 lazy register 注释，讲座以用户态补登记为正道，不把内核 fallback 当主线。
""")

    def s_book():
        s = add()
        head(s, "四　GDR", "登记本上的三笔", len(slides))
        rows = [
            ("Alloc 0x503c", "NV50_THIRD_PARTY_P2P。挂在 subdevice 下。类型取 BAR1。"),
            ("0x503c0102", "REGISTER_VA_SPACE。声明这本本子管哪一个 GPU VA space。"),
            ("0x503c0104", "REGISTER_VIDMEM。写入 hMemory、VA、size。地址与长度须 64K 对齐。"),
        ]
        y = 1.22
        for a, b in rows:
            frame(s, 0.55, y, 12.25, 1.35)
            tb(s, 0.80, y, 3.3, 1.35, a, 16, RED, True, mono=True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 4.20, y, 8.35, 1.35, b, 15, INK, anchor=MSO_ANCHOR.MIDDLE)
            y += 1.45
        tb(s, 0.55, 5.70, 12.2, 1.15,
           "hMemory 须是可登记的显存对象，通常是 class 0x40（NV01_MEMORY_LOCAL_USER）。\n"
           "普通 cuMemAlloc 走 UVM 时，RM 里往往没有这张档案，故后文要把分配换成 VMM。",
           14, FAINT)
        notes(s, """
三笔对应 Tesla libcuda 会发、GeForce 不会发的 ioctl。后面 hook 就是补这三笔。
""")

    def s_harry():
        s = add()
        head(s, "五　借鉴", "Harry 的 5090：闸也在用户态，但那是另一扇门", len(slides))
        tb(s, 0.55, 1.18, 12.2, 0.40,
           "harrychen.xyz/2026/05/20/enable-gpudirect-rdma-on-rtx-5090/",
           13, FAINT, mono=True)
        rows = [
            ("内核 dma-buf 能力位置上，CUDA 仍报 0。", "与本稿相同的教训：驱动点头，libcuda 可以继续摇头。"),
            ("属性 116 看的是 libcuda 内部 bit，不是当场问内核。", "进程内拨开 0x20，属性变为 1，dma-buf 导出开始工作。"),
            ("最终：拷一份 libcuda，or $0x40 改为 or $0x60。", "文件偏移随版本变。打开的是 dma-buf / NCCL。"),
            ("作者写明：这不会让旧 nvidia-peermem 工作。", "旧路径查的是 0x503c 登记，不是这个 bit。"),
        ]
        y = 1.65
        for a, b in rows:
            frame(s, 0.55, y, 12.25, 1.20)
            tb(s, 0.75, y + 0.12, 6.0, 0.96, a, 14, INK, True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 6.85, y + 0.12, 5.75, 0.96, b, 14, FAINT, anchor=MSO_ANCHOR.MIDDLE)
            y += 1.26
        notes(s, """
借鉴：不要再为 GDR 打 NVIDIA 内核；116 是软件旗。
不照抄：一字节不是 peermem 方案。我们要 get_pages。
""")

    def s_borrow():
        s = add()
        head(s, "五　借鉴", "三条来源，各借一层", len(slides))
        cols = [
            ("Harry", "闸在用户态。不要把内核能力位当成最后一关。"),
            ("remote-lab / mcornea", "ioctl 观察 RM handle，自建 NV503C，REGISTER_VIDMEM。"),
            ("本稿", "VMM 替换、dlsym、SYNC_MEMOPS 801、isolate 运行；不改 libcuda 文件。"),
        ]
        for i, (t, b) in enumerate(cols):
            frame(s, 0.55 + i * 4.20, 1.25, 4.05, 4.20)
            tb(s, 0.75 + i * 4.20, 1.45, 3.65, 0.70, t, 16, RED, True, title=True)
            tb(s, 0.75 + i * 4.20, 2.25, 3.65, 2.90, b, 15, INK, spacing=1.25)
        tb(s, 0.55, 5.65, 12.2, 1.20,
           "早期只骗属性 116：GDRCopy 肯 pin，get_pages 仍 0x57。\n"
           "普通 cuMemAlloc 后登记：常常没有 class 0x40 的 hMemory。只 hook ioctl、不 hook dlsym：urma_perftest 绕过 PLT。",
           14, FAINT)
        notes(s, """
弯路压在页脚两句即可，不必另开颂歌式「弯路」页。
""")

    def s_hook_pos():
        s = add()
        head(s, "六　补登记", "用户态补的是手续，不是第二条 DMA", len(slides))
        tb(s, 0.55, 1.20, 12.2, 1.00,
           "libgdr_geforce_hook.so 用 LD_PRELOAD 插入进程。不改 libcuda 文件，不改 NVIDIA 内核。",
           15, INK)
        rows = [
            ("观察", "拦截 ioctl。仅当请求形如 RM_ALLOC 且 fd 是 NVIDIA RM（major 195）时，在成功之后抄 handle。"),
            ("改分配", "cuMemAlloc 改为 VMM（Create / Map）。RM 会 Alloc class 0x40，才能登记。"),
            ("补写", "Alloc 0x503c，REGISTER_VA_SPACE，REGISTER_VIDMEM。自己发的 ioctl 走 libc 真函数，避免递归。"),
            ("两扇小门", "属性 116/110 返回 1，使 GDRCopy 通过能力检查；VMM 上 SYNC_MEMOPS 的 801 按成功返回。"),
        ]
        y = 2.30
        for a, b in rows:
            frame(s, 0.55, y, 12.25, 1.08)
            tb(s, 0.75, y, 2.1, 1.08, a, 15, RED, True, title=True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 2.95, y, 9.6, 1.08, b, 14, INK, anchor=MSO_ANCHOR.MIDDLE)
            y += 1.12
        notes(s, """
四行是 hook 的全部职责。不要用「骗换看补」这种字。
""")

    def s_preload():
        s = add()
        head(s, "六　补登记", "符号拦截，以及 dlopen 那一条岔路", len(slides))
        frame(s, 0.55, 1.22, 12.25, 2.40)
        tb(s, 0.80, 1.40, 11.8, 2.05,
           "动态链接器先加载本 .so。导出与 libc / libcuda 同名的 ioctl、cuMemAlloc_v2 等，调用先进入本文件。\n"
           "真实现用 dlsym(RTLD_NEXT, ...) 取得。不得再调用自己导出的同名函数。\n"
           "静态链接或直接 syscall(SYS_ioctl) 则套不住。575 上的 GDRCopy 走动态符号。",
           15, INK, spacing=1.22)
        frame(s, 0.55, 3.80, 12.25, 2.95, BOX2)
        tb(s, 0.80, 4.00, 11.8, 2.55,
           "urma_perftest 自己 dlopen(libcuda) 再 dlsym(handle, \"cuMemAlloc_v2\")，不经 PLT。\n"
           "故本文件也拦截 dlsym：当 name 是我们劫持的符号且 handle 不是 RTLD_NEXT 时，把钩子指针还回去。\n"
           "内部查真符号一律走 real_dlsym / dlvsym。",
           15, INK, spacing=1.22)
        notes(s, """
LD_PRELOAD 不是改文件偏移。对比 Harry 的一字节。
sudo 会丢掉该环境变量。用 isolate 的 run.sh。
""")

    def s_ioctl():
        s = add()
        head(s, "六　补登记", "ioctl：先完成，再抄房间号", len(slides))
        frame(s, 0.55, 1.20, 12.25, 3.55, CODE_BG)
        tb(s, 0.80, 1.35, 11.8, 3.25, [
            ("int ioctl(int fd, unsigned long request, ...)", 14, CODE_FG, False, True),
            ("    if (!is_rm_alloc_ioctl(request))          // magic 'F', nr 0x2B/0x28", 13, CODE_FG, False, True),
            ("        return real_ioctl(...);               // 立即转发", 13, CODE_FG, False, True),
            ("    ret = real_ioctl(...);                    // 先做真事", 13, CODE_FG, False, True),
            ("    if (ret == 0 && is_nvidia_rm_fd(fd))", 13, CODE_FG, False, True),
            ("        after_rm_alloc(...);                  // 成功后再记账", 13, CODE_FG, False, True),
            ("    return ret;", 13, CODE_FG, False, True),
        ])
        tb(s, 0.55, 4.95, 12.2, 1.85,
           "不改工单内容。CUDA 仍以为自己在与内核说话。\n"
           "非 NVIDIA 的 fd（socket、tty、framebuffer 虽也可能用 'F'）不拆包。\n"
           "class 是房间类型（0x80 设备、0x2080 子设备、0x90f1 VA space、0x40 显存）；handle 每次运行不同。",
           15, INK, spacing=1.2)
        notes(s, """
对着 gdr_geforce_hook.c 约 540 行。强调事后抄号，不是篡改请求。
""")

    def s_vmm():
        s = add()
        head(s, "六　补登记", "分配改走 VMM，为的是 class 0x40", len(slides))
        rows = [
            ("Reserve", "cuMemAddressReserve，64K 对齐的 GPU VA。"),
            ("Create", "cuMemCreate。打开 capturing_memory，ioctl 钩子记下 hMemory。"),
            ("Map", "cuMemMap + SetAccess。门牌与档案对应。"),
            ("Register", "把 (hMemory, VA, size) 写入 0x503c。"),
        ]
        for i, (a, b) in enumerate(rows):
            frame(s, 0.55 + i * 3.20, 1.25, 3.05, 2.85)
            tb(s, 0.70 + i * 3.20, 1.45, 2.75, 0.45, f"{i+1}  {a}", 16, RED, True, title=True)
            tb(s, 0.70 + i * 3.20, 2.10, 2.75, 1.75, b, 14, INK, spacing=1.2)
        tb(s, 0.55, 4.35, 12.2, 2.45,
           "gpuDirectRDMACapable=1 在 GeForce 上常使 cuMemCreate 返回 101。去掉该旗再试，物理页仍能建，登记不受阻。\n"
           "该旗表示「申报这段内存适合 GDR」，不是「无此旗便不能写入 0x503c」。\n"
           "GDRCopy 若自己走 VMM，cuMemCreate / cuMemMap 的钩子同样登记。",
           15, INK, spacing=1.22)
        notes(s, """
普通 cuMemAlloc 走 UVM，REGISTER_VIDMEM 会报 no hMemory。这是换 VMM 的唯一理由。
""")

    def s_small_doors():
        s = add()
        head(s, "六　补登记", "能力检查与 SYNC_MEMOPS", len(slides))
        frame(s, 0.55, 1.22, 6.00, 5.55)
        tb(s, 0.80, 1.42, 5.55, 0.40, "属性 116 / 110", 16, RED, True, title=True)
        tb(s, 0.80, 2.00, 5.55, 4.4,
           "GDRCopy 的 check_gdr_support() 读它们。\n"
           "Harry 拨 libcuda 里的 bit；本稿挟持\n"
           "cuDeviceGetAttribute，把返回值写成 1。\n\n"
           "只做这一步，get_pages 仍是 0x57。\n"
           "它只让应用肯进入 pin。",
           15, INK, spacing=1.25)
        frame(s, 6.75, 1.22, 6.05, 5.55, BOX2)
        tb(s, 7.00, 1.42, 5.6, 0.40, "SYNC_MEMOPS 801", 16, RED, True, title=True)
        tb(s, 7.00, 2.00, 5.6, 4.4,
           "pin 前 GDRCopy 会\n"
           "cuPointerSetAttribute(SYNC_MEMOPS)。\n"
           "4090 上对 VMM 指针常返回 801。\n"
           "库把它当作致命错误。\n\n"
           "若 VA 由本 hook 分配，返回 SUCCESS。\n"
           "登记成功却死在 copylat，多半是这里。",
           15, INK, spacing=1.25)
        notes(s, """
两扇都是应用层检查，不是 BAR1，也不是登记本本身。
""")

    def s_logs():
        s = add()
        head(s, "七　验证", "成功时日志的顺序，即本场的顺序", len(slides))
        frame(s, 0.55, 1.20, 12.25, 4.55, CODE_BG)
        tb(s, 0.80, 1.38, 11.8, 4.20, [
            ("[gdr-geforce] ready GPU 0 client=... device=... subdev=... va=...", 13, CODE_FG, False, True),
            ("[gdr-geforce] cuMemCreate RDMA-capable failed (101), retry without flag", 13, CODE_DIM, False, True),
            ("[gdr-geforce] captured hMemory=0x... class=0x40", 13, CODE_FG, False, True),
            ("[gdr-geforce] allocated NV50_THIRD_PARTY_P2P hTPP=...", 13, CODE_FG, False, True),
            ("[gdr-geforce] REGISTER_VA_SPACE ... status=0x0", 13, CODE_FG, False, True),
            ("[gdr-geforce] REGISTER_VIDMEM  ... status=0x0", 13, CODE_FG, False, True),
            ("[gdr-geforce] cuMemAlloc -> VMM va=0x...", 13, CODE_FG, False, True),
            ("# 此后 gdr_pin_buffer 不得为 -22", 13, CODE_DIM, False, True),
        ])
        tb(s, 0.55, 5.95, 12.2, 0.90,
           "101 那一行不是失败。handle 与 VA 每次运行不同，不要写进代码。",
           14, FAINT)
        notes(s, """
按行对应：家谱 → VMM → 建本 → 写 VA → 写 vidmem → 返回给应用。
""")

    def s_compare():
        s = add()
        head(s, "七　验证", "两条工作对照", len(slides))
        headers = ["", "Harry · 5090", "本稿 · 4090 D"]
        rows = [
            ["目标", "NCCL + dma-buf", "GDRCopy / peermem / get_pages"],
            ["内核", "试过改 dma-buf 能力，非闸门", "只保留 BAR1 P2P flag 与大 BAR1"],
            ["用户态", "改 libcuda 一字节", "LD_PRELOAD .so，不改文件"],
            ["打开的层", "能力 bit / dma-buf 导出", "attr 116 + 0x503c 登记"],
            ["加载", "LD_LIBRARY_PATH 指向拷贝", "isolate/run.sh，禁止全局 preload"],
        ]
        y = 1.20
        cols, ws = [0.55, 3.15, 8.05], [2.6, 4.9, 4.75]
        rect(s, 0.55, y, 12.25, 0.50, BOX2, RULE, 0.8)
        for i, h in enumerate(headers):
            tb(s, cols[i], y, ws[i], 0.50, h, 13, RED, True, title=True, anchor=MSO_ANCHOR.MIDDLE)
        y = 1.70
        for i, row in enumerate(rows):
            bg = BOX if i % 2 == 0 else CREAM
            rect(s, 0.55, y, 12.25, 0.95, bg, RULE, 0.55)
            for j, cell in enumerate(row):
                tb(s, cols[j], y, ws[j], 0.95, cell, 14, INK, bold=(j == 0),
                   anchor=MSO_ANCHOR.MIDDLE)
            y += 0.95
        notes(s, """
两份工作不矛盾，路径不同。不要在现场叠两套用户态手法。
""")

    def s_run():
        s = add()
        head(s, "七　验证", "运行范围", len(slides))
        frame(s, 0.55, 1.22, 12.25, 2.55, CODE_BG)
        tb(s, 0.80, 1.40, 11.8, 2.20, [
            ("./isolate_umdk_gpu.sh build $HOME/umdk_gpu_isolated", 14, CODE_FG, False, True),
            ("$HOME/umdk_gpu_isolated/run.sh \\", 14, CODE_FG, False, True),
            ("  .../urma_perftest write_lat_gpu --gpu-mode=peermem ...", 14, CODE_FG, False, True),
            ("# UMDK_GDR_HOOK=0  可临时去掉钩子", 13, CODE_DIM, False, True),
        ])
        tb(s, 0.55, 4.00, 12.2, 2.80,
           "run.sh 只给这一次 exec 设置 LD_PRELOAD。打开脚本或 cat 不会加载 .so。\n"
           "不要全局 export，不要写 /etc/ld.so.preload。sudo 会丢掉该变量；已是 root 则不要再套 sudo。\n"
           "源码：userspace/gdr-geforce-hook/gdr_geforce_hook.c，文件头注释即加载模型。",
           15, INK, spacing=1.22)
        notes(s, """
操作只留这一页。强调隔离。
""")

    def s_trouble():
        s = add()
        head(s, "七　验证", "日志缺哪一句，回到哪一层", len(slides))
        rows = [
            ("无 ready GPU", "ioctl 未套上，或尚未 cuInit", "符号拦截 / run.sh"),
            ("cannot alloc 0x503c", "家谱未齐", "ioctl 记账"),
            ("no hMemory", "仍走普通 cuMemAlloc", "VMM"),
            ("REGISTER_VIDMEM != 0", "handle 错或未 64K 对齐", "登记三笔"),
            ("登记成功，pin 仍 -22", "pin 的 VA 不在登记范围", "对照日志 va/size"),
            ("死在 801", "SYNC_MEMOPS 被当成致命", "两扇小门"),
        ]
        y = 1.18
        rect(s, 0.55, y, 12.25, 0.42, BOX2, RULE, 0.7)
        tb(s, 0.70, y, 3.8, 0.42, "现象", 12, RED, True, title=True, anchor=MSO_ANCHOR.MIDDLE)
        tb(s, 4.55, y, 4.5, 0.42, "含义", 12, RED, True, title=True, anchor=MSO_ANCHOR.MIDDLE)
        tb(s, 9.10, y, 3.5, 0.42, "层", 12, RED, True, title=True, anchor=MSO_ANCHOR.MIDDLE)
        y = 1.60
        for i, (a, b, c) in enumerate(rows):
            bg = BOX if i % 2 == 0 else CREAM
            rect(s, 0.55, y, 12.25, 0.82, bg, RULE, 0.5)
            tb(s, 0.70, y, 3.8, 0.82, a, 13, INK, True, mono=True, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 4.55, y, 4.5, 0.82, b, 14, INK, anchor=MSO_ANCHOR.MIDDLE)
            tb(s, 9.10, y, 3.5, 0.82, c, 14, FAINT, anchor=MSO_ANCHOR.MIDDLE)
            y += 0.82
        notes(s, """
答问索引。几乎所有现场问题都能指回「窗 / 申报 / 登记」三层之一。
""")

    def s_bound():
        s = add()
        head(s, "八　边界", "本稿止于 pin 成功", len(slides))
        frame(s, 0.55, 1.25, 6.00, 5.50)
        tb(s, 0.80, 1.45, 5.55, 0.40, "已说明", 16, RED, True, title=True)
        tb(s, 0.80, 2.05, 5.55, 4.4,
           "P2P 与 GDR 不是一词。\n"
           "二者共用 BAR1 与 p2pGetCaps。\n"
           "flag 打开的是连通性申报。\n"
           "GDR 在内核侧因此可直接适配。\n"
           "GeForce libcuda 仍不写 0x503c。\n"
           "用户态 .so 补登记后，get_pages 不再 0x57。",
           15, INK, spacing=1.28)
        frame(s, 6.75, 1.25, 6.05, 5.50, BOX2)
        tb(s, 7.00, 1.45, 5.60, 0.40, "未说明", 16, RED, True, title=True)
        tb(s, 7.00, 2.05, 5.60, 4.4,
           "UMMU、隔离 SVA、MATT、4K SG。\n"
           "那是 pin 成功之后，把页交给本所网卡驱动的事。\n\n"
           "也不把 Harry 的 dma-buf 路径\n"
           "讲成 peermem 的替代证明。",
           15, INK, spacing=1.28)
        notes(s, """
有人问 UMMU：明确是下一场。验收只有 pin 不再 -22，以及日志里 REGISTER_VIDMEM status=0x0。
""")

    def s_end():
        s = add()
        set_bg(s)
        rect(s, 0.55, 0.55, 12.25, 0.018, RULE)
        tb(s, 0.55, 1.10, 12.2, 0.40, "文献与代码", 14, RED, True, title=True)
        tb(s, 0.55, 1.60, 12.2, 0.70, "三层，由下而上", 26, INK, True, title=True)
        tb(s, 0.55, 2.50, 12.2, 3.4,
           "窗：BAR1。GPU P2P 与 GDR 的数据面。\n"
           "申报：p2pGetCaps / PCIE_BAR1。只改 flag 即可选用现成实现。\n"
           "登记：0x503c。Tesla 的 libcuda 写，GeForce 不写；本稿在用户态补。\n\n"
           "源码  userspace/gdr-geforce-hook/gdr_geforce_hook.c\n"
           "内核注释  src/nvidia/src/kernel/platform/p2p/p2p_caps.c  （两类对象同一检查）\n"
           "导出 API  kernel-open/nvidia/nv-p2p.h\n"
           "对照  harrychen.xyz/2026/05/20/enable-gpudirect-rdma-on-rtx-5090/",
           15, INK, spacing=1.28)
        footer(s, len(slides), "文献")
        notes(s, """
收束用三层，不要用口号。提问先请对方说清问的是窗、申报，还是登记。
Q: 算不算攻击？A: 只对自愿 preload 的测试进程拆 RM_ALLOC。不要全局安装。
Q: 5090 能否用这 so？A: 若目标是 GDRCopy/peermem，手续相同；若目标是 NCCL dma-buf，Harry 的路径对症。
""")

    fns = [
        s_title, s_confusion, s_terms, s_table, s_pcie, s_bar01, s_window,
        s_gpup2p, s_conn, s_shared_gate, s_policy, s_flags, s_enough, s_barsize,
        s_machine, s_gdr_def, s_get_pages, s_adapt, s_why_fail, s_book, s_harry,
        s_borrow, s_hook_pos, s_preload, s_ioctl, s_vmm, s_small_doors, s_logs,
        s_compare, s_run, s_trouble, s_bound, s_end,
    ]
    TOTAL[0] = len(fns)
    for fn in fns:
        fn()
    prs.save(OUT)
    print(f"wrote {OUT} ({TOTAL[0]} slides)")


if __name__ == "__main__":
    build()
