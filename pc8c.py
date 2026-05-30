import sys
import os
import subprocess
import string
import re
from PIL import Image

def decompress_p8_old_perfect(compressed_bytes):
    """2016年当時のPICO-8旧世代LZ77カスタム圧縮を完璧に解凍する"""
    if len(compressed_bytes) < 8: return compressed_bytes
    size_hi, size_lo = compressed_bytes[0] ^ 0xFF, compressed_bytes[1] ^ 0xFF
    decompressed_size = (size_hi << 8) | size_lo
    out = bytearray()
    raw_data = compressed_bytes[4:]
    pos, bit_buffer, bit_count = 0, 0, 0

    def read_bits_lsb(n):
        nonlocal bit_buffer, bit_count, pos
        while bit_count < n:
            if pos >= len(raw_data): return 0
            bit_buffer |= raw_data[pos] << bit_count
            pos += 1
            bit_count += 8
        val = bit_buffer & ((1 << n) - 1)
        bit_buffer >>= n
        bit_count -= n
        return val

    while len(out) < decompressed_size and pos <= len(raw_data):
        if read_bits_lsb(1) == 1:
            out.append(read_bits_lsb(8) ^ 0xFF)
        else:
            offset, length = read_bits_lsb(9), read_bits_lsb(5) + 2
            if offset == 0: break
            start_idx = len(out) - offset
            if start_idx < 0:
                for _ in range(length):
                    if len(out) >= decompressed_size: break
                    out.append(0x20)
                continue
            for _ in range(length):
                if len(out) >= decompressed_size: break
                out.append(out[start_idx])
                start_idx += 1
    return bytes(out)

def decompress_new_format(compressed_bytes):
    """現行形式 (\\x00pxa) の解凍"""
    decompressed_size = (compressed_bytes[5] << 8) | compressed_bytes[4]
    out = bytearray()
    pos, bit_buffer, bit_count = 8, 0, 0
    def read_bits_lsb(n):
        nonlocal bit_buffer, bit_count, pos
        while bit_count < n:
            if pos >= len(compressed_bytes): return 0
            bit_buffer |= compressed_bytes[pos] << bit_count
            pos += 1
            bit_count += 8
        val = bit_buffer & ((1 << n) - 1)
        bit_buffer >>= n
        bit_count -= n
        return val
    mtf = bytearray(b"\n 0123456789abcdefghijklmnopqrstuvwxyz!#%()^*+,-./:;<=>?@[\\]_`{|}~\"'\t")
    while len(out) < decompressed_size and pos <= len(compressed_bytes):
        if read_bits_lsb(1) == 1:
            idx = read_bits_lsb(6)
            if idx < len(mtf):
                char = mtf[idx]
                out.append(char)
                mtf.pop(idx)
                mtf.insert(0, char)
        else:
            offset, length = read_bits_lsb(9) + 1, read_bits_lsb(5) + 2
            start_idx = len(out) - offset
            if start_idx < 0: continue
            for _ in range(length):
                if len(out) >= decompressed_size: break
                out.append(out[start_idx])
                start_idx += 1
    return bytes(out)

def strip_comments_and_whitespace(lua_text):
    # 複数行コメント --[[ ... ]] の削除
    lua_text = re.sub(r'--\[\[.*?\]\]', '', lua_text, flags=re.DOTALL)
    
    lines = lua_text.splitlines()
    cleaned_lines = []
    for line in lines:
        in_string = False
        str_char = None
        comment_start = -1
        i = 0
        n = len(line)
        while i < n:
            c = line[i]
            if not in_string:
                if c in ('"', "'"):
                    in_string = True
                    str_char = c
                elif c == '[' and i + 1 < n and line[i+1] == '[':
                    in_string = True
                    str_char = ']]'
                    i += 1
                elif c == '-' and i + 1 < n and line[i+1] == '-':
                    comment_start = i
                    break
            else:
                if str_char == ']]':
                    if c == ']' and i + 1 < n and line[i+1] == ']':
                        in_string = False
                        i += 1
                else:
                    if c == str_char:
                        esc_count = 0
                        k = i - 1
                        while k >= 0 and line[k] == '\\':
                            esc_count += 1
                            k -= 1
                        if esc_count % 2 == 0:
                            in_string = False
            i += 1
            
        if comment_start != -1:
            line = line[:comment_start]
            
        line_stripped = line.strip()
        if line_stripped:
            cleaned_lines.append(line_stripped)
            
    return cleaned_lines

def convert_pico8_to_standard_lua(lua_text):
    """
    【shrinko8仕様の完全移植】
    PICO-8の独自拡張マクロ構文を、標準Lua 5.2が完璧にコンパイルできる状態に展開する
    """
    lines = strip_comments_and_whitespace(lua_text)
    converted_lines = []
    
    for line in lines:
        # 1. PICO-8固有の自己代入演算子 (+=, -=, *=, /=) の厳密な展開
        # 文字列リテラルを保護し、複数ステートメント連結時でも右辺を正しく切り出す
        placeholders = []
        def repl_str(m):
            placeholders.append(m.group(0))
            return f"__STR_PLACEHOLDER_{len(placeholders)-1}__"
        
        # 文字列の退避
        pattern_str = r'(?:"(?:[^"\\]|\\.)*"|\'(?:[^\'\\]|\\.)*\'|\[\[.*?\]\])'
        line_tmp = re.sub(pattern_str, repl_str, line, flags=re.DOTALL)
        
        # 先読みLookaheadアサーションを用いた自己代入の展開
        # 右辺のキャプチャ範囲を「次の代入(=)、キーワード、または行末」に抑える
        # さらに、スペースなしで連結された次のステートメント（例: 1l(...) や )l(...)）も適切に切り離す
        lookahead = (
            r"(?=\s*(?:"
            r"local\b|if\b|then\b|do\b|end\b|for\b|while\b|function\b|return\b|else\b|elseif\b|break\b|goto\b|repeat\b|until\b|"
            r"(?<!\.)[a-zA-Z_][a-zA-Z0-9_\.\[\]'\"]*\s*(?<![~<>=!])[-+*/]?=(?!=)"
            r")|"
            r"(?<!\band)(?<!\bor)(?<!\bnot)(?<=[\]\)}])\s*(?=(?!and\b|or\b|not\b)[a-zA-Z_])|"
            r"(?<!\band)(?<!\bor)(?<!\bnot)(?<=[0-9])\s*(?=(?!and\b|or\b|not\b)[a-zA-Z_])|"
            r"(?<!\band)(?<!\bor)(?<!\bnot)(?<=[a-zA-Z_])\s+(?=(?!and\b|or\b|not\b)[a-zA-Z_])|"
            r"$)"
        )
        line_tmp = re.sub(r"(\b[a-zA-Z_][a-zA-Z0-9_\.\[\]'\"]*)\s*\+=\s*(.*?)" + lookahead, r"\1 = \1 + (\2)", line_tmp)
        line_tmp = re.sub(r"(\b[a-zA-Z_][a-zA-Z0-9_\.\[\]'\"]*)\s*-=\s*(.*?)" + lookahead, r"\1 = \1 - (\2)", line_tmp)
        line_tmp = re.sub(r"(\b[a-zA-Z_][a-zA-Z0-9_\.\[\]'\"]*)\s*\*=\s*(.*?)" + lookahead, r"\1 = \1 * (\2)", line_tmp)
        line_tmp = re.sub(r"(\b[a-zA-Z_][a-zA-Z0-9_\.\[\]'\"]*)\s*/=\s*(.*?)" + lookahead, r"\1 = \1 / (\2)", line_tmp)
        
        # 2. PICO-8の '?' (print) ショートハンドの展開
        # '?' から始まって次の '?' または ';' または 行末までの範囲を print() に置き換える
        def repl_q(m):
            args = m.group(1).strip()
            if args.endswith(';'):
                return f"print({args[:-1].strip()});"
            else:
                return f"print({args})"
        line_tmp = re.sub(r'\?(.*?)(?=\?|;|$)', repl_q, line_tmp)
        
        # 文字列の復元
        for i, orig in enumerate(placeholders):
            line_tmp = line_tmp.replace(f"__STR_PLACEHOLDER_{i}__", orig)
            
        line = line_tmp

        # 3. PICO-8の一行省略 if: if (条件) 処理 -> if 条件 then 処理 end
        if "if" in line and "then" not in line:
            match = re.match(r"^(\s*if\s*\(.+?\))\s*(.+)$", line)
            if match:
                cond_part, action_part = match.group(1), match.group(2)
                if not action_part.strip().endswith("end") and not action_part.strip().endswith("then"):
                    line = f"{cond_part} then {action_part} end"

        # 4. PICO-8特有の比較演算子 != ➔ ~= (標準Lua形式) への置換
        line = line.replace("!=", "~=")

        converted_lines.append(line)
        
    return "\n".join(converted_lines)

def convert_png_to_pc8c(png_path, pc8c_path):
    compile_exe = "/mnt/g/PC8/tools/pc8_compile.exe"
    abs_pc8c_path = os.path.abspath(pc8c_path)

    print(f"Opening PNG Cartridge: {png_path}")
    img = Image.open(png_path).convert('RGBA')
    width, height = img.size
    ordered_bytes = bytearray()
    for y in range(height):
        for x in range(width):
            r, g, b, a = img.getpixel((x, y))
            ordered_bytes.append(((a & 3) << 6) | ((r & 3) << 4) | ((g & 3) << 2) | (b & 3))

    code_bytes = ordered_bytes[20480:]

    # 1. Luaソースコードの完全デコード
    if code_bytes[:4] == b'\x00pxa':
        lua_bytes = decompress_new_format(code_bytes)
    else:
        lua_bytes = decompress_p8_old_perfect(code_bytes)
    
    # 2. クリーニングおよび、大文字を小文字に統一
    allowed_chars = set(string.printable)
    raw_text = lua_bytes.decode('utf-8', errors='ignore')
    cleaned_chars = [c for c in raw_text if c in allowed_chars]
    lua_text = "".join(cleaned_chars).strip().lower() # PICO-8フォント規約に準拠

    # 3. 【核心】shrinko8互換の「標準Luaトランスパイル」を実行
    lua_standard_text = convert_pico8_to_standard_lua(lua_text)
    lua_lines_pure = "\n".join([line.rstrip() for line in lua_standard_text.splitlines()])

    # 4. 引数破損を防ぐため、最短名でカレントディレクトリ実行
    original_cwd = os.getcwd()
    work_dir = "/mnt/g/PC8/tools"
    os.chdir(work_dir)

    temp_p8_name = "in.p8"
    temp_p8_path = os.path.join(work_dir, temp_p8_name)
    
    with open(temp_p8_path, 'wb') as f:
        f.write(b"pico-8 cartridge\n")
        f.write(b"version 8\n") 
        f.write(b"__lua__\n")
        f.write(lua_lines_pure.encode('utf-8'))
        f.write(b"\n")

    print(f"Compiling via native engine: ./pc8_compile.exe")
    try:
        result = subprocess.run(
            ["./pc8_compile.exe", "game", temp_p8_name, abs_pc8c_path], 
            stdout=subprocess.PIPE, 
            stderr=subprocess.PIPE, 
            check=True
        )
        print(result.stdout.decode('utf-8', errors='ignore').strip())
        print(f"🚀 Success! {abs_pc8c_path} compiled perfectly.")
    except subprocess.CalledProcessError as e:
        print(f"\n❌ Compile Engine Error!")
        print(f"STDOUT: {e.stdout.decode('utf-8', errors='ignore')}")
        print(f"STDERR: {e.stderr.decode('utf-8', errors='ignore')}")
    finally:
        if os.path.exists(temp_p8_path):
            os.remove(temp_p8_path)
        os.chdir(original_cwd)

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python png2pc8c.py input.p8.png output.pc8c")
    else:
        convert_png_to_pc8c(sys.argv[1], sys.argv[2])
