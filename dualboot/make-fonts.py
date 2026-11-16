from pathlib import Path
import sys
import os
from PIL import Image, ImageDraw, ImageFont
from datetime import datetime
from zoneinfo import ZoneInfo
import json
p=Path(__file__).resolve().parent
stamp=datetime.now(ZoneInfo('America/New_York')).strftime('%Y-%m-%d %H:%M %Z')
footer='v0.1.120  |  '+stamp
texts={60:['Multi-Boot Selector'],34:['by sun12'],44:['Android','Linux','Reboot','Power Off'],24:[footer],22:['Volume + / - to select   |   Power to confirm','Android is not available in this build','Starting Linux...','Starting Android...','Unpacking failed. Select another option.','Rebooting...','Powering off...','Power off did not complete. Try disconnecting USB.','Reboot: preparing reset','Reboot: reset requested','Reboot: reset unavailable','Elapsed 0123456789. s','Android selected','Linux selected','Decompressing kernel','Checking kernel CRC32','Kernel verified. Preparing handoff','Entering kernel']}
header=['/* Rasterized Segoe UI glyphs for the fixed boot-menu strings. */','#include "types.h"']
fonts={}
for size,strings in texts.items():
 font=ImageFont.truetype(os.environ.get('T5_MENU_FONT', 'C:/Windows/Fonts/segoeui.ttf'),size)
 height=size+14
 chars=sorted(set(''.join(strings)))
 data=[];metrics=[]
 for code in range(32,127):
  ch=chr(code)
  if ch not in chars:metrics.append((0,0));continue
  width=int(font.getlength(ch)+1)
  im=Image.new('L',(width,height));ImageDraw.Draw(im).text((0,0),ch,font=font,fill=255)
  metrics.append((len(data),width));data.extend(im.tobytes())
 header.append('static const u8 font%d_data[]={%s};'%(size,','.join(map(str,data))))
 header.append('static const struct glyph font%d_glyphs[]={%s};'%(size,','.join('{%d,%d}'%x for x in metrics)))
 fonts[size]=font
header.append('#define BUILD_TEXT "%s"'%footer)
p.joinpath('fonts.h').write_text('\n'.join(header),encoding='ascii')
p.joinpath('build-date.json').write_text(json.dumps({'version':'v0.1.120','build_date':stamp,'footer':footer}))
