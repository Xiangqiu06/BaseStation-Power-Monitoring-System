#!/usr/bin/env bash
#===================================================================
# 生成界面用的中文字体
#
# 为什么要自己生成：
#   LVGL 自带的 LV_FONT_SIMSUN_16_CJK 只有 1000 个常用字，
#   "湿/雾/阈/池" 这些监测系统的常用字反而不在里面，界面上会显示成方块。
#   用 lv_font_conv 从系统黑体里只抠出界面用到的字，一个 16px 字库
#   只有几十 KB，比整字库（几 MB）省得多。
#
# 用法（在项目根目录执行）：
#   ./tools/gen_font.sh
#
# 依赖：
#   node/npm（用 npx 临时拉 lv_font_conv，不需要全局安装）
#   Windows 下用 C:/Windows/Fonts/simhei.ttf，
#   Linux 下把 FONT 改成 /usr/share/fonts/.../wqy-zenhei.ttc 之类
#===================================================================
set -e

FONT="${FONT:-C:/Windows/Fonts/simhei.ttf}"
OUT_DIR="$(dirname "$0")/../ui/fonts"
LV_FONT_CONV="npx --yes lv_font_conv@1.5.2"

if [ ! -f "$FONT" ]; then
    echo "找不到字体文件: $FONT"
    echo "可以用 FONT=/path/to/xxx.ttf ./tools/gen_font.sh 指定"
    exit 1
fi

# 从源码里扫出所有中文字符，保证界面上的字一个都不缺
CHARSET=$(python - <<'PY'
import io, re, glob
chars = set()
for p in glob.glob('src/*.c'):
    s = io.open(p, encoding='utf-8').read()
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    for ch in s:
        if ord(ch) > 127:
            chars.add(ch)
print("".join(sorted(chars)))
PY
)

echo "字库字符数: $(echo -n "$CHARSET" | wc -m)"

# 16px：标签、表格、正文
$LV_FONT_CONV --font "$FONT" --size 16 --bpp 4 --format lvgl \
    --range 0x20-0x7E --symbols "$CHARSET" \
    -o "$OUT_DIR/ui_font_cn_16.c"

# 28px：大字号数值，只放数字和符号，省空间
$LV_FONT_CONV --font "$FONT" --size 28 --bpp 4 --format lvgl \
    --range 0x20-0x7E \
    -o "$OUT_DIR/ui_font_cn_28.c"

ls -lh "$OUT_DIR"
echo "字体生成完成"
