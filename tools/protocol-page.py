#!/usr/bin/env python3
"""Render docs/protocol.md as a standalone HTML page (used for the shared
claude.ai artifact).  Usage: pandoc docs/protocol.md -f gfm -t html5 --wrap=none
| tools/protocol-page.py > page.html"""
import re
import sys

body = sys.stdin.read()
body = re.sub(r'<h1[^>]*>.*?</h1>\s*', '', body, count=1, flags=re.S)
m = re.match(r'\s*<p>(.*?)</p>', body, flags=re.S)
lede = m.group(1); body = body[m.end():]
m = re.match(r'\s*<p>(Conventions:.*?)</p>', body, flags=re.S)
conv = m.group(1); body = body[m.end():]

nav = []
for lvl, i, t in re.findall(r'<h([23]) id="([^"]+)">(.*?)</h\1>', body):
    t = re.sub('<[^>]+>', '', t)
    if lvl == '2':
        nav.append(f'<li class="l2"><a href="#{i}">{re.sub(r"^Part (\d): ", "", t)}</a></li>')
    else:
        nav.append(f'<li class="l3"><a href="#{i}">{t}</a></li>')
navhtml = '\n'.join(nav)
body = body.replace('<hr />', '<hr>')
body = body.replace('<table>', '<div class="tbl"><table>').replace('</table>', '</table></div>')

print(f'''<title>FT-891 to AH-4 Protocol</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:ital,wght@0,400;0,500;0,600;1,400&family=IBM+Plex+Sans+Condensed:wght@500;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
:root {{
  --paper:#f5f7f9; --panel:#ffffff; --ink:#1b222b; --ink-2:#4a5562; --ink-3:#7b8692;
  --rule:#d6dce3; --accent:#0b5fae; --accent-ink:#0b5fae; --accent-soft:#e3eef9;
  --signal:#b3541e; --code-bg:#eef1f4; --code-ink:#1f2a36;
  --font-body:"IBM Plex Sans",system-ui,-apple-system,"Segoe UI",sans-serif;
  --font-display:"IBM Plex Sans Condensed","IBM Plex Sans",system-ui,sans-serif;
  --font-mono:"IBM Plex Mono",ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;
}}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{
  color-scheme:dark; --paper:#13181e; --panel:#1a2129; --ink:#e3e8ed; --ink-2:#aab4bf; --ink-3:#7f8a96;
  --rule:#2c3640; --accent:#6fb1ff; --accent-ink:#8cc0ff; --accent-soft:#1a2a3d;
  --signal:#f0975b; --code-bg:#1f272f; --code-ink:#d9e0e7;
}} }}
:root[data-theme="dark"] {{
  color-scheme:dark; --paper:#13181e; --panel:#1a2129; --ink:#e3e8ed; --ink-2:#aab4bf; --ink-3:#7f8a96;
  --rule:#2c3640; --accent:#6fb1ff; --accent-ink:#8cc0ff; --accent-soft:#1a2a3d;
  --signal:#f0975b; --code-bg:#1f272f; --code-ink:#d9e0e7;
}}
* {{ box-sizing:border-box; }}
body {{ margin:0; background:var(--paper); color:var(--ink); font-family:var(--font-body); font-size:16px; line-height:1.55; }}
a {{ color:var(--accent-ink); text-decoration:none; }}
a:hover, a:focus-visible {{ text-decoration:underline; outline:none; }}
:focus-visible {{ outline:2px solid var(--accent); outline-offset:2px; }}
.wrap {{ max-width:1180px; margin:0 auto; padding-inline:20px; padding-block:0 64px; }}
header.top {{ border-bottom:1px solid var(--rule); padding-block:36px 28px; }}
.eyebrow {{ font-family:var(--font-mono); font-size:12px; letter-spacing:.08em; text-transform:uppercase; color:var(--ink-3); margin:0 0 10px; }}
header.top h1 {{ font-family:var(--font-display); font-weight:600; font-size:clamp(28px,4vw,44px); line-height:1.1; margin:0 0 14px; text-wrap:balance; letter-spacing:-.01em; }}
.lede {{ max-width:68ch; color:var(--ink-2); margin:0 0 14px; }}
.conv {{ max-width:68ch; font-size:14px; color:var(--ink-3); margin:0; }}
.legend {{ display:flex; flex-wrap:wrap; gap:10px 18px; margin-top:18px; font-size:13px; color:var(--ink-2); font-family:var(--font-mono); }}
.legend span::before {{ content:""; display:inline-block; width:10px; height:10px; border-radius:2px; margin-right:7px; vertical-align:-1px; }}
.legend .m::before {{ background:var(--accent); }}
.legend .r::before {{ background:var(--signal); }}
.layout {{ display:grid; grid-template-columns:230px minmax(0,1fr); gap:48px; align-items:start; }}
nav.toc {{ position:sticky; top:calc(env(safe-area-inset-top,0px) + 16px); max-height:calc(100vh - 32px); overflow:auto; padding-block:20px 8px; font-size:13.5px; }}
nav.toc ul {{ list-style:none; margin:0; padding:0; }}
nav.toc li.l2 {{ margin-top:14px; font-family:var(--font-display); font-weight:600; font-size:14px; text-transform:uppercase; letter-spacing:.04em; }}
nav.toc li.l3 {{ margin:5px 0 0 0; padding-left:12px; border-left:2px solid var(--rule); color:var(--ink-2); }}
nav.toc li.l3 a {{ color:inherit; }}
main {{ min-width:0; padding-top:8px; }}
main > h2 {{ font-family:var(--font-display); font-weight:600; font-size:28px; margin:56px 0 8px; padding-top:24px; border-top:3px solid var(--ink); text-wrap:balance; }}
main > h2:first-child {{ margin-top:32px; }}
main h3 {{ font-family:var(--font-display); font-weight:600; font-size:20px; margin:36px 0 10px; text-wrap:balance; }}
main h4 {{ font-family:var(--font-display); font-weight:600; font-size:16px; margin:26px 0 8px; color:var(--ink-2); }}
main p, main li {{ max-width:72ch; }}
main p {{ margin:0 0 14px; }}
main ul, main ol {{ padding-left:22px; margin:0 0 14px; }}
main li {{ margin-bottom:6px; }}
main strong {{ font-weight:600; }}
code {{ font-family:var(--font-mono); font-size:.9em; background:var(--code-bg); color:var(--code-ink); padding:1px 5px; border-radius:3px; }}
pre {{ font-family:var(--font-mono); font-size:13px; line-height:1.5; background:var(--code-bg); color:var(--code-ink); padding:14px 16px; border-radius:6px; overflow-x:auto; margin:0 0 18px; border-left:3px solid var(--accent); }}
pre code {{ background:none; padding:0; font-size:inherit; }}
.tbl {{ overflow-x:auto; margin:0 0 20px; border:1px solid var(--rule); border-radius:6px; background:var(--panel); }}
table {{ border-collapse:collapse; width:100%; font-size:14px; }}
th, td {{ text-align:left; vertical-align:top; padding:8px 12px; border-bottom:1px solid var(--rule); }}
th {{ font-family:var(--font-display); font-weight:600; font-size:13px; letter-spacing:.03em; text-transform:uppercase; color:var(--ink-2); background:var(--accent-soft); }}
tr:last-child td {{ border-bottom:none; }}
td code, th code {{ white-space:nowrap; }}
td {{ font-variant-numeric:tabular-nums; }}
hr {{ border:0; border-top:1px solid var(--rule); margin:40px 0; }}
footer.foot {{ border-top:1px solid var(--rule); margin-top:48px; padding-top:16px; font-size:13px; color:var(--ink-3); font-family:var(--font-mono); }}
@media (max-width: 860px) {{
  .layout {{ grid-template-columns:1fr; gap:8px; }}
  nav.toc {{ position:static; max-height:none; border:1px solid var(--rule); border-radius:6px; padding:12px 16px 16px; background:var(--panel); }}
  nav.toc li.l2:first-child {{ margin-top:0; }}
}}
@media (prefers-reduced-motion: no-preference) {{ html {{ scroll-behavior:smooth; }} }}
</style>
<div class="wrap">
<header class="top">
  <p class="eyebrow">Implementer's reference · measured on an FT-891 and an AH-4, October 2026</p>
  <h1>Yaesu TUN/LIN tuner protocol and the Icom AH-4 interface</h1>
  <p class="lede">{lede}</p>
  <p class="conv">{conv}</p>
  <div class="legend"><span class="m">measured: observed on the bench</span><span class="r">reported: from other people's work</span></div>
</header>
<div class="layout">
<nav class="toc" aria-label="Contents"><ul>
{navhtml}
</ul></nav>
<main>
{body}
</main>
</div>
<footer class="foot">Source: github.com/ast/yaesu-ah4, docs/protocol.md. Distilled from design-notes.md sections 8 to 10. SM6WJM / AD8KM.</footer>
</div>''')
