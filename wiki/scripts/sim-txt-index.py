"""Host simulation of TxtReaderActivity::buildPageIndex / loadPageAtOffset (src/activities/reader/TxtReaderActivity.cpp @ f460af3).

Counts work units instead of time:
  glyph_ops   - codepoints walked by getTextAdvanceX (each = utf8 decode + ligature + 2 kern-class + interval lookups)
  measure_calls, heap_allocs (std::string/vector temporaries), sd_bytes, file_opens, vTaskDelay(1) calls.
Glyph widths are approximated (space=5px, other=10px); viewport ~ 528px => ~52 chars/line, 27 lines/page.
"""
import sys

CHUNK = 8 * 1024
VIEW_W = 528
LINES_PER_PAGE = 27


def adv(ch):
    return 5 if ch == " " else 10


class Stats:
    def __init__(self):
        self.glyph_ops = self.measure_calls = self.allocs = 0
        self.sd_bytes = self.opens = self.delays = self.pages = 0


def text_width(s, st):
    st.measure_calls += 1
    st.glyph_ops += len(s)
    return sum(adv(c) for c in s)


def find_wrap_break(text, st):
    st.allocs += 1  # utf8Boundaries vector
    st.glyph_ops += 0  # boundary scan is cheap byte work, not counted as glyph ops
    bounds = list(range(1, len(text) + 1))
    lo, hi = 0, len(bounds)
    while lo < hi:
        mid = (lo + hi + 1) // 2
        st.allocs += 1  # text.substr(0, bytes)
        if text_width(text[: bounds[mid - 1]], st) <= VIEW_W:
            lo = mid
        else:
            hi = mid - 1
    max_fit = bounds[0] if lo == 0 else bounds[lo - 1]
    if max_fit < len(text):
        sp = text.rfind(" ", 0, max_fit)
        if sp > 0:
            return sp
    return max_fit


def load_page(data, offset, st, wrap_steps):
    size = len(data)
    chunk = min(CHUNK, size - offset)
    st.opens += 1
    st.sd_bytes += chunk
    st.allocs += 1  # malloc(chunk)
    buf = data[offset : offset + chunk]
    pos, out = 0, 0
    while pos < chunk and out < LINES_PER_PAGE:
        end = buf.find("\n", pos)
        if end == -1:
            end = chunk
        complete = end < chunk or offset + end >= size
        if not complete and out > 0:
            break
        line = buf[pos:end].rstrip("\r")
        st.allocs += 1
        consumed = 0
        while line and out < LINES_PER_PAGE:
            if text_width(line, st) <= VIEW_W:
                out += 1
                st.allocs += 1
                line = ""
                break
            bp = find_wrap_break(line, st)
            if bp == 0:
                break
            out += 1
            st.allocs += 2  # push substr + line = line.substr
            skip = bp + (1 if bp < len(line) and line[bp] == " " else 0)
            consumed += skip
            line = line[skip:]
            wrap_steps[0] += 1
            if wrap_steps[0] & 0x0F == 0:
                st.delays += 1
        if not line:
            pos = end + 1
        else:
            pos += consumed
            break
    if pos == 0 and out:
        pos = 1
    return min(offset + pos, size), out > 0


def main(path):
    data = open(path, encoding="utf-8", errors="replace").read()
    st = Stats()
    offset, pages = 0, 1
    while offset < len(data):
        wrap = [0]  # wrapSteps is local to loadPageAtOffset
        nxt, ok = load_page(data, offset, st, wrap)
        if not ok or nxt <= offset:
            break
        offset = nxt
        if offset < len(data):
            pages += 1
            if pages & 0x07 == 0:
                st.delays += 1
    chars = len(data)
    print(f"{path}")
    print(f"  chars={chars} pages={pages}")
    print(f"  glyph_ops={st.glyph_ops:,}  ({st.glyph_ops / chars:.1f} per char)")
    print(f"  measure_calls={st.measure_calls:,}  heap_allocs~{st.allocs:,}")
    print(f"  sd_read={st.sd_bytes / 1e6:.1f} MB ({st.sd_bytes / chars:.1f}x file) file_opens={st.opens}")
    print(f"  vTaskDelay(1)={st.delays}")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
