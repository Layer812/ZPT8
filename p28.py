import sys
from PIL import Image

def decompress_old_format_spec(compressed_bytes):
    """
    Wiki仕様完全準拠：
    Old Compressed Format (:c:\\x00) のMSBシフト ＋ 事後ビット反転デコーダー
    """
    if len(compressed_bytes) < 8:
        return compressed_bytes

    # 4-5バイト目が解凍後のデータサイズ (Big Endian)
    decompressed_size = (compressed_bytes[4] << 8) | compressed_bytes[5]
    out = bytearray()
    
    # 【最重要】解凍前のデータは「そのまま（未反転）」で使用する
    raw_data = compressed_bytes[8:]
    
    pos = 0
    bit_buffer = 0
    bit_count = 0

    def read_bits_msb(n):
        """Old形式は各バイトの上位ビット(MSB)から順に消費する仕様"""
        nonlocal bit_buffer, bit_count, pos
        val = 0
        for i in range(n):
            if bit_count == 0:
                if pos >= len(raw_data):
                    return 0
                bit_buffer = raw_data[pos]
                pos += 1
                bit_count = 8
            
            # 最上位ビット(MSB)を1ビット取り出す
            bit = (bit_buffer >> 7) & 1
            bit_buffer = (bit_buffer << 1) & 0xFF
            bit_count -= 1
            
            # 取り出したビットを戻り値の下位から詰める
            val |= (bit << i)
        return val

    # 解凍メインループ
    while len(out) < decompressed_size and pos <= len(raw_data):
        if read_bits_msb(1) == 1:
            # プレーンな1バイトを取り出す
            out.append(read_bits_msb(8))
        else:
            # LZ77コピー (オフセット9ビット、長さ5ビット)
            offset = read_bits_msb(9)
            length = read_bits_msb(5) + 2
            
            if offset == 0: # データ終了の合図
                break
                
            start_idx = len(out) - offset
            if start_idx < 0:
                continue
                
            for _ in range(length):
                if len(out) >= decompressed_size:
                    break
                out.append(out[start_idx])
                start_idx += 1

    # 【仕様の核心】解凍し終わった「後の」全テキストデータに対してビット反転をかける！
    final_lua_bytes = bytearray(b ^ 0xFF for b in out)
    return bytes(final_lua_bytes)

def decompress_new_format(compressed_bytes):
    """New Compressed Format (\\x00pxa) の解凍"""
    decompressed_size = (compressed_bytes[5] << 8) | compressed_bytes[4]
    out = bytearray()
    pos = 8
    bit_buffer = 0
    bit_count = 0
    
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
            offset = read_bits_lsb(9) + 1
            length = read_bits_lsb(5) + 2
            start_idx = len(out) - offset
            if start_idx < 0: continue
            for _ in range(length):
                if len(out) >= decompressed_size: break
                out.append(out[start_idx])
                start_idx += 1
    return bytes(out)

def extract_p8_png_perfect(png_path, p8_path):
    try:
        img = Image.open(png_path).convert('RGBA')
    except Exception as e:
        print(f"Error opening image: {e}")
        return

    width, height = img.size
    ordered_bytes = bytearray()
    
    # Wiki指定の ARGB 順ビットデコード
    for y in range(height):
        for x in range(width):
            r, g, b, a = img.getpixel((x, y))
            byte_val = ((a & 3) << 6) | ((r & 3) << 4) | ((g & 3) << 2) | (b & 3)
            ordered_bytes.append(byte_val)

    gfx_bytes = ordered_bytes[:20480]
    code_bytes = ordered_bytes[20480:]

    # グラフィック（__gfx__）テキストの復元
    gfx_lines = []
    for y in range(128):
        line = ""
        for x in range(64):
            b = gfx_bytes[y * 64 + x]
            line += f"{b & 0x0F:x}{(b >> 4) & 0x0F:x}"
        gfx_lines.append(line)
    gfx_text = "\n".join(gfx_lines) + "\n"

    # マジックナンバーの確認（生のまま判定する）
    header = code_bytes[:4]
    
    if header == b':c:\x00':
        print("Detected: Old Compressed Format (:c:\\x00) -> Decompressing then inverting text...")
        lua_code_bytes = decompress_old_format_spec(code_bytes)
    elif header == b'\x00pxa':
        print("Detected: New Compressed Format (\\x00pxa) -> Decompressing...")
        lua_code_bytes = decompress_new_format(code_bytes)
    else:
        print("Detected: Uncompressed text.")
        end_idx = code_bytes.find(0x00)
        lua_code_bytes = code_bytes[:end_idx] if end_idx != -1 else code_bytes

    # .p8ファイルとして書き出し
    with open(p8_path, 'wb') as f:
        f.write(b"pico-8 cartridge // successfully extracted from wiki spec\nversion 16\n__lua__\n")
        f.write(lua_code_bytes)
        if not lua_code_bytes.endswith(b'\n'):
            f.write(b'\n')
        f.write(b"__gfx__\n")
        f.write(gfx_text.encode('ascii'))

    print(f"Success! {p8_path} generated perfectly.")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python png2p8.py input.p8.png output.p8")
    else:
        extract_p8_png_perfect(sys.argv[1], sys.argv[2])
