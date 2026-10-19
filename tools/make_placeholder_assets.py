#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
生成 VCC 客户端 UI 占位图（4 张），**零第三方依赖**。

用途
----
原始素材（193 张表情 + 37 张 UI 图）不随源码发布，版权归属不明。
但 `logindialog.ui` / `vccdialog.ui` 里共 6 处通过 `resource.qrc`
引用了其中 4 张图，若文件缺失，编译能过但运行时按钮与登录背景为空白。

本脚本生成同尺寸、同名、同用途的中性占位图，保证：
  - 克隆仓库后可直接 qmake + make 编译通过；
  - 界面元素完整显示，不出现空白按钮；
  - 不引入任何第三方素材版权风险。

实现
----
只用 Python 标准库（`zlib` + `struct`）直接编码 PNG，
不依赖 Pillow —— 免去使用者安装额外包的步骤。

用法
----
  python tools/make_placeholder_assets.py

输出
----
  vcc-client/images/register.png   478x854   登录页品牌背景（显示区 220x400）
  vcc-client/images/sett.png       975x975   设置按钮图标
  vcc-client/images/++.png         2048x2048 「+」号按钮图标（被引用 3 处）
  vcc-client/tx/4.png              79x77     摄像头按钮图标
"""

import math
import os
import struct
import sys
import zlib

# ---------- 配色：深蓝灰主色 + 浅色前景，与视频会议场景相符 ----------
BG_TOP = (37, 52, 74)
BG_BOTTOM = (24, 34, 49)
FG = (226, 234, 244)
ACCENT = (74, 144, 226)
CLEAR = (0, 0, 0, 0)


# ==================================================================
#  极简 RGBA 画布 + PNG 编码器
# ==================================================================

class Canvas:
    """纯 Python 的 RGBA 画布，坐标以左上为原点。"""

    __slots__ = ("w", "h", "buf")

    def __init__(self, w, h, fill=CLEAR):
        self.w = w
        self.h = h
        self.buf = bytearray(bytes(fill) * (w * h))

    # ---------- 基础 ----------

    def px(self, x, y, c):
        """写单个像素，坐标越界自动忽略。"""
        x, y = int(x), int(y)
        if 0 <= x < self.w and 0 <= y < self.h:
            i = (y * self.w + x) * 4
            self.buf[i:i + 4] = bytes(c)

    def blend(self, x, y, c):
        """Alpha 混合写入，用于边缘抗锯齿。"""
        x, y = int(x), int(y)
        if not (0 <= x < self.w and 0 <= y < self.h):
            return
        a = c[3] / 255.0
        if a >= 1.0:
            self.px(x, y, c)
            return
        if a <= 0.0:
            return
        i = (y * self.w + x) * 4
        for k in range(3):
            self.buf[i + k] = int(self.buf[i + k] * (1 - a) + c[k] * a)
        self.buf[i + 3] = int(self.buf[i + 3] * (1 - a) + 255 * a)

    # ---------- 填充 ----------

    def fill_rect(self, x0, y0, x1, y1, c):
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                self.blend(x, y, c)

    def fill_rrect(self, x0, y0, x1, y1, r, c):
        """圆角矩形填充（含 4 个角的圆弧区域）。"""
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                dx = 0
                if x < x0 + r:
                    dx = (x0 + r) - x
                elif x >= x1 - r:
                    dx = x - (x1 - r - 1)
                dy = 0
                if y < y0 + r:
                    dy = (y0 + r) - y
                elif y >= y1 - r:
                    dy = y - (y1 - r - 1)
                if dx * dx + dy * dy <= r * r:
                    self.blend(x, y, c)

    def stroke_rrect(self, x0, y0, x1, y1, r, t, c):
        """圆角矩形描边（空心）。"""
        for y in range(int(y0), int(y1)):
            for x in range(int(x0), int(x1)):
                dx = 0
                if x < x0 + r:
                    dx = (x0 + r) - x
                elif x >= x1 - r:
                    dx = x - (x1 - r - 1)
                dy = 0
                if y < y0 + r:
                    dy = (y0 + r) - y
                elif y >= y1 - r:
                    dy = y - (y1 - r - 1)
                if dx * dx + dy * dy <= r * r:
                    self.blend(x, y, c)

    def fill_circle(self, cx, cy, r, c):
        """实心圆填充，带 1px 抗锯齿。"""
        for y in range(int(cy - r - 1), int(cy + r + 2)):
            for x in range(int(cx - r - 1), int(cx + r + 2)):
                d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
                if d <= r:
                    self.blend(x, y, c)
                elif d <= r + 1:                      # 边缘半透明过渡
                    self.blend(x, y, (c[0], c[1], c[2], int(255 * (r + 1 - d))))

    def stroke_circle(self, cx, cy, r, t, c):
        """圆环描边。"""
        ro, ri = r + t / 2.0, r - t / 2.0
        for y in range(int(cy - ro - 1), int(cy + ro + 2)):
            for x in range(int(cx - ro - 1), int(cx + ro + 2)):
                d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
                if ri <= d <= ro:
                    edge = min(d - ri, ro - d)
                    a = int(255 * min(1.0, edge + 0.5))
                    self.blend(x, y, (c[0], c[1], c[2], a))

    def stroke_arc(self, cx, cy, r, a0, a1, t, c):
        """圆弧描边，角度单位为度，0° 在正右方，顺时针为正（屏幕坐标）。"""
        for step in range(0, 361):
            ang = a0 + (a1 - a0) * step / 360.0
            rad = math.radians(ang)
            for w in range(int(t)):
                rr = r - t / 2.0 + w
                self.blend(cx + math.cos(rad) * rr, cy + math.sin(rad) * rr, c)

    def fill_poly(self, pts, c):
        """凸/凹多边形扫描线填充。"""
        ys = [p[1] for p in pts]
        for y in range(int(min(ys)), int(max(ys)) + 1):
            xs = []
            n = len(pts)
            for i in range(n):
                x1, y1 = pts[i]
                x2, y2 = pts[(i + 1) % n]
                if (y1 <= y < y2) or (y2 <= y < y1):
                    xs.append(x1 + (y + 0.5 - y1) * (x2 - x1) / (y2 - y1))
            xs.sort()
            for i in range(0, len(xs) - 1, 2):
                for x in range(int(math.floor(xs[i])), int(math.ceil(xs[i + 1]))):
                    self.blend(x, y, c)

    def vgradient(self, c0, c1):
        """垂直线性渐变铺满全画布。"""
        for y in range(self.h):
            t = y / max(self.h - 1, 1)
            c = (
                int(c0[0] + (c1[0] - c0[0]) * t),
                int(c0[1] + (c1[1] - c0[1]) * t),
                int(c0[2] + (c1[2] - c0[2]) * t),
                255,
            )
            i = y * self.w * 4
            self.buf[i:i + self.w * 4] = bytes(c) * self.w

    # ---------- 输出 ----------

    def save(self, path):
        """编码为 8 位 RGBA PNG 并写盘。"""
        raw = bytearray()
        stride = self.w * 4
        for y in range(self.h):
            raw.append(0)                                   # 过滤器类型 0
            raw += self.buf[y * stride:(y + 1) * stride]

        def chunk(tag, data):
            out = struct.pack(">I", len(data)) + tag + data
            return out + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

        png = b"\x89PNG\r\n\x1a\n"
        png += chunk(b"IHDR", struct.pack(">IIBBBBB", self.w, self.h, 8, 6, 0, 0, 0))
        png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        png += chunk(b"IEND", b"")
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(png)


# ==================================================================
#  占位位图字体（5x7 点阵，纯标准库环境下显示文字）
# ==================================================================

# 标准 5x7 ASCII 点阵字库，覆盖 A-Z / 0-9 / 空格。
_FONT_SRC = {
    " ": "00000 00000 00000 00000 00000 00000 00000",
    "A": "01110 10001 10001 11111 10001 10001 10001",
    "B": "11110 10001 10001 11110 10001 10001 11110",
    "C": "01111 10000 10000 10000 10000 10000 01111",
    "D": "11110 10001 10001 10001 10001 10001 11110",
    "E": "11111 10000 10000 11110 10000 10000 11111",
    "F": "11111 10000 10000 11110 10000 10000 10000",
    "G": "01111 10000 10000 10011 10001 10001 01111",
    "H": "10001 10001 10001 11111 10001 10001 10001",
    "I": "01110 00100 00100 00100 00100 00100 01110",
    "J": "00111 00010 00010 00010 00010 10010 01100",
    "K": "10001 10010 10100 11000 10100 10010 10001",
    "L": "10000 10000 10000 10000 10000 10000 11111",
    "M": "10001 11011 10101 10101 10001 10001 10001",
    "N": "10001 10001 11001 10101 10011 10001 10001",
    "O": "01110 10001 10001 10001 10001 10001 01110",
    "P": "11110 10001 10001 11110 10000 10000 10000",
    "Q": "01110 10001 10001 10001 10101 10010 01101",
    "R": "11110 10001 10001 11110 10100 10010 10001",
    "S": "01111 10000 10000 01110 00001 00001 11110",
    "T": "11111 00100 00100 00100 00100 00100 00100",
    "U": "10001 10001 10001 10001 10001 10001 01110",
    "V": "10001 10001 10001 10001 10001 01010 00100",
    "W": "10001 10001 10001 10101 10101 11011 10001",
    "X": "10001 10001 01010 00100 01010 10001 10001",
    "Y": "10001 10001 01010 00100 00100 00100 00100",
    "Z": "11111 00001 00010 00100 01000 10000 11111",
    "0": "01110 10001 10011 10101 11001 10001 01110",
    "1": "00100 01100 00100 00100 00100 00100 01110",
    "2": "01110 10001 00001 00010 00100 01000 11111",
    "3": "11111 00010 00100 00010 00001 10001 01110",
    "4": "00010 00110 01010 10010 11111 00010 00010",
    "5": "11111 10000 11110 00001 00001 10001 01110",
    "6": "00110 01000 10000 11110 10001 10001 01110",
    "7": "11111 00001 00010 00100 01000 01000 01000",
    "8": "01110 10001 10001 01110 10001 10001 01110",
    "9": "01110 10001 10001 01111 00001 00010 01100",
    "-": "00000 00000 00000 11111 00000 00000 00000",
    ".": "00000 00000 00000 00000 00000 01100 01100",
    "/": "00001 00010 00010 00100 01000 01000 10000",
    ":": "00000 01100 01100 00000 01100 01100 00000",
}

GLYPH_W, GLYPH_H, SPACING = 5, 7, 1


def draw_text(cv, x0, y0, text, scale, color):
    """用内置 5x7 点阵绘制文字（自动转大写，未收录字符跳过）。"""
    cx = x0
    for ch in text.upper():
        g = _FONT_SRC.get(ch)
        if g is None:
            cx += (GLYPH_W + SPACING) * scale
            continue
        for ry, row in enumerate(g.split()):
            for rx, bit in enumerate(row):
                if bit == "1":
                    cv.fill_rect(cx + rx * scale, y0 + ry * scale,
                                 cx + (rx + 1) * scale, y0 + (ry + 1) * scale, color)
        cx += (GLYPH_W + SPACING) * scale
    return cx


def text_width(text, scale):
    return len(text) * (GLYPH_W + SPACING) * scale


# ==================================================================
#  四张占位图的绘制逻辑
# ==================================================================

def make_register(path):
    """登录页左侧品牌背景。原图 478x854，UI 中缩放显示为 220x400。"""
    cv = Canvas(478, 854)
    cv.vgradient(BG_TOP, BG_BOTTOM)

    # 顶部细高光条，暗示主界面标题栏
    cv.fill_rect(0, 0, 478, 4, ACCENT + (255,))

    # 中部同心圆，代表「会议 / 连接」意象
    cx, cy = 239, 340
    cv.stroke_circle(cx, cy, 150, 3, ACCENT + (255,))
    cv.stroke_circle(cx, cy, 108, 2, (58, 84, 112, 255))
    cv.fill_circle(cx, cy, 64, (44, 62, 86, 255))

    # 中心摄像头意象：矩形机身 + 镜头
    cv.fill_rrect(cx - 46, cy - 26, cx + 46, cy + 26, 10, FG + (255,))
    cv.fill_poly([(cx + 50, cy - 16), (cx + 78, cy - 30), (cx + 78, cy + 30), (cx + 50, cy + 16)],
                 FG + (255,))

    # 文字区：VCC 用点阵字（scale 6）
    s = 6
    draw_text(cv, cx - text_width("VCC", s) // 2, 548, "VCC", s, FG + (255,))

    # 品牌标：两条不等长横线，兼作分隔与视觉重心
    cv.fill_rect(cx - 150, 606, cx + 150, 609, ACCENT + (255,))
    cv.fill_rect(cx - 90, 617, cx + 90, 620, (58, 84, 112, 255))

    # 底部英文副标题（原「视频会议」的英文等价物，避免依赖中文字形）
    s2 = 3
    sub = "VIDEO CONFERENCE"
    draw_text(cv, cx - text_width(sub, s2) // 2, 660, sub, s2, (150, 175, 205, 255))

    s3 = 2
    tag = "PLACEHOLDER ASSET"
    draw_text(cv, cx - text_width(tag, s3) // 2, 706, tag, s3, (110, 132, 160, 255))

    # 底部细线收尾
    cv.fill_rect(cx - 70, 762, cx + 70, 764, (58, 84, 112, 255))

    cv.save(path)
    return cv.w, cv.h


def make_sett(path):
    """设置按钮图标。原图 975x975，齿轮造型。"""
    S = 975
    cv = Canvas(S, S)
    cx = cy = S // 2

    # 底盘
    cv.fill_circle(cx, cy, 400, BG_TOP + (255,))

    # 8 个齿
    for i in range(8):
        rad = math.radians(i * 45 - 90)
        cv.fill_circle(cx + math.cos(rad) * 352, cy + math.sin(rad) * 352, 80, BG_TOP + (255,))

    # 齿之间的凹口（用背景透明色挖出，形成齿轮轮廓）
    for i in range(8):
        rad = math.radians(i * 45 - 67.5)
        cv.fill_circle(cx + math.cos(rad) * 372, cy + math.sin(rad) * 372, 52, CLEAR)

    # 内圈 + 中心孔
    cv.fill_circle(cx, cy, 252, ACCENT + (255,))
    cv.fill_circle(cx, cy, 106, BG_TOP + (255,))
    cv.stroke_circle(cx, cy, 106, 4, (24, 34, 49, 255))

    cv.save(path)
    return cv.w, cv.h


def make_plus(path):
    """「+」号按钮图标。原图 2048x2048，被 vccdialog.ui 引用 3 处。"""
    S = 2048
    cv = Canvas(S, S)
    cx = cy = S // 2

    # 圆形底 + 高亮描边
    cv.fill_circle(cx, cy, 900, BG_TOP + (255,))
    cv.stroke_circle(cx, cy, 888, 26, ACCENT + (255,))

    # 圆角加号：横臂 + 竖臂
    arm, t = 430, 150
    cv.fill_rrect(cx - arm, cy - t // 2, cx + arm, cy + t // 2, t // 2 - 20, FG + (255,))
    cv.fill_rrect(cx - t // 2, cy - arm, cx + t // 2, cy + arm, t // 2 - 20, FG + (255,))

    cv.save(path)
    return cv.w, cv.h


def make_tx4(path):
    """主界面功能按钮图标。原图 79x77，小尺寸需按像素手工绘制。"""
    W, H = 79, 77
    cv = Canvas(W, H)

    # 圆角方框
    cv.fill_rrect(1, 1, W - 1, H - 1, 12, BG_TOP + (255,))
    cv.stroke_rrect(1, 1, W - 1, H - 1, 12, 2, ACCENT + (255,))

    # 摄像头造型：矩形机身 + 右侧梯形镜头
    cv.fill_rrect(15, 26, 49, 51, 5, FG + (255,))
    cv.fill_poly([(51, 33), (63, 26), (63, 51), (51, 44)], FG + (255,))

    # 上方信号弧（三段递减）
    for r in (52, 44, 36):
        cv.stroke_arc(39, 40, r, 208, 332, 2, ACCENT + (255,))

    # 底部三脚架小点，暗示「设备」
    for x in (30, 39, 48):
        cv.fill_rect(x, 62, x + 2, 66, (128, 150, 178, 255))

    cv.save(path)
    return cv.w, cv.h


TARGETS = [
    (os.path.join("vcc-client", "images", "register.png"), make_register),
    (os.path.join("vcc-client", "images", "sett.png"),     make_sett),
    (os.path.join("vcc-client", "images", "++.png"),       make_plus),
    (os.path.join("vcc-client", "tx", "4.png"),             make_tx4),
]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    print("占位图生成 —— 尺寸与原图一致，纯标准库实现，无第三方依赖\n")
    total = 0
    for rel, fn in TARGETS:
        path = os.path.join(root, rel)
        w, h = fn(path)
        size = os.path.getsize(path)
        total += size
        print(f"  {rel:28s} {w}x{h:<6d} {size:>9,d} 字节")
    print(f"\n完成，共 {len(TARGETS)} 张，合计 {total:,d} 字节。")
    print("这些是自绘中性占位图，不含任何第三方素材。")


if __name__ == "__main__":
    main()