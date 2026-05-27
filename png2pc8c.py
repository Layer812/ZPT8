import sys
import os
import subprocess
import shutil

def find_shrinko8():
    """shrinko8の実行可能ファイルまたはスクリプトを自動検出する"""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    
    # 1. tools/shrinko8/shrinko8.py (Pythonソースコード版)
    tools_py = os.path.join(script_dir, "tools", "shrinko8", "shrinko8.py")
    if os.path.exists(tools_py):
        return [sys.executable, tools_py]

    # 2. スクリプトと同じディレクトリの shrinko8.py
    local_py = os.path.join(script_dir, "shrinko8.py")
    if os.path.exists(local_py):
        return [sys.executable, local_py]

    # 3. スクリプトと同じディレクトリの shrinko8.exe
    local_exe = os.path.join(script_dir, "shrinko8.exe")
    if os.path.exists(local_exe):
        return [local_exe]

    # 4. システムのPATHにある shrinko8
    path_exe = shutil.which("shrinko8")
    if path_exe:
        return [path_exe]

    # 5. システムのPATHにある shrinko8.exe
    path_exe_win = shutil.which("shrinko8.exe")
    if path_exe_win:
        return [path_exe_win]

    return None

def find_pc8_compile():
    """pc8_compileのバイナリを自動検出する"""
    script_dir = os.path.dirname(os.path.abspath(__file__))
    exe_name = "pc8_compile.exe" if os.name == 'nt' else "pc8_compile"
    
    tools_exe = os.path.join(script_dir, "tools", exe_name)
    if os.path.exists(tools_exe):
        return tools_exe
        
    root_exe = os.path.join(script_dir, exe_name)
    if os.path.exists(root_exe):
        return root_exe

    path_exe = shutil.which(exe_name)
    if path_exe:
        return path_exe

    return None

def convert_png_to_pc8c(png_path, pc8c_path):
    abs_png_path = os.path.abspath(png_path)
    abs_pc8c_path = os.path.abspath(pc8c_path)

    shrinko8_cmd = find_shrinko8()
    if not shrinko8_cmd:
        print("[ERROR] shrinko8 が見つかりません。")
        print("        ルートディレクトリに 'shrinko8.exe' を配置するか、PATHに通してください。")
        return False

    pc8_compile_path = find_pc8_compile()
    if not pc8_compile_path:
        print("[ERROR] pc8_compile バイナリが見つかりません。")
        print("        'tools/pc8_compile.exe' がビルドされているか確認してください。")
        return False

    temp_p8_path = abs_pc8c_path + ".temp.p8"

    print("Step 1: Extracting PNG cartridge using shrinko8...")
    print(f"        Cmd: {' '.join(shrinko8_cmd)} {png_path} -> temp.p8")
    
    try:
        subprocess.run(
            shrinko8_cmd + [abs_png_path, temp_p8_path],
            check=True
        )
    except subprocess.CalledProcessError as e:
        print(f"[ERROR] shrinko8 でのデコードに失敗しました。終了コード: {e.returncode}")
        if os.path.exists(temp_p8_path):
            os.remove(temp_p8_path)
        return False

    if not os.path.exists(temp_p8_path):
        print("[ERROR] shrinko8 が一時 .p8 ファイルを出力しませんでした。")
        return False

    print("Step 2: Compiling to PC8C using pc8_compile...")
    print(f"        Cmd: {pc8_compile_path} game {os.path.basename(temp_p8_path)} {os.path.basename(pc8c_path)}")

    work_dir = os.path.dirname(pc8_compile_path)
    original_cwd = os.getcwd()
    
    clean_temp_name = "in_temp.p8"
    clean_temp_path = os.path.join(work_dir, clean_temp_name)

    success = False
    try:
        # PICO-8独自の `if ... do` 構文を標準的な `if ... then` に置換してコピーする
        with open(temp_p8_path, 'r', encoding='utf-8', errors='ignore') as rf:
            content = rf.read()
        
        parts = content.split('__lua__\n')
        if len(parts) > 1:
            header = parts[0]
            lua_part = parts[1]
            next_sec_idx = -1
            for sec_marker in ['__gfx__\n', '__map__\n', '__gff__\n', '__music__\n', '__sfx__\n', '__label__\n']:
                idx = lua_part.find(sec_marker)
                if idx != -1:
                    if next_sec_idx == -1 or idx < next_sec_idx:
                        next_sec_idx = idx
            
            if next_sec_idx != -1:
                lua_code = lua_part[:next_sec_idx]
                footer = lua_part[next_sec_idx:]
            else:
                lua_code = lua_part
                footer = ""
            
            import re
            # PICO-8独自の if/elseif ... do 構文を標準的な then に置換
            pattern = r'(?<![a-zA-Z_])(if|elseif)\b((?:(?!\b(then|do|for|while)\b).)+?)(?<![a-zA-Z_])do\b'
            lua_code_mod = re.sub(pattern, r'\1\2then', lua_code)
            
            new_content = header + '__lua__\n' + lua_code_mod + '\n' + footer
            with open(clean_temp_path, 'w', encoding='utf-8', errors='ignore') as wf:
                wf.write(new_content)
        else:
            shutil.copy(temp_p8_path, clean_temp_path)
            
        os.chdir(work_dir)
        
        result = subprocess.run(
            [os.path.basename(pc8_compile_path), "game", clean_temp_name, abs_pc8c_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=True
        )
        
        stdout_str = result.stdout.decode('utf-8', errors='ignore').strip()
        if stdout_str:
            print(stdout_str)
            
        print(f"\n[SUCCESS] 変換成功! {os.path.basename(pc8c_path)} が作成されました。")
        success = True
        
    except subprocess.CalledProcessError as e:
        print("\n[ERROR] コンパイルエンジンエラー!")
        print(f"STDOUT: {e.stdout.decode('utf-8', errors='ignore')}")
        print(f"STDERR: {e.stderr.decode('utf-8', errors='ignore')}")
    finally:
        if os.path.exists(clean_temp_path):
            os.remove(clean_temp_path)
        if os.path.exists(temp_p8_path):
            os.remove(temp_p8_path)
        os.chdir(original_cwd)

    return success

if __name__ == "__main__":
    # WindowsコンソールでのUnicodeEncodeError対策として、標準出力をutf-8で再オープンする
    # (ただし環境によってはうまく動作しない場合があるため、表示自体から絵文字を排除しています)
    if hasattr(sys.stdout, 'reconfigure'):
        try:
            sys.stdout.reconfigure(encoding='utf-8')
        except Exception:
            pass

    if len(sys.argv) < 2:
        print("使用方法: python png2pc8c.py <input.p8.png> [output.pc8c]")
        print("例: python png2pc8c.py cutebunnies-0.p8.png")
        sys.exit(1)
        
    png_in = sys.argv[1]
    
    if len(sys.argv) >= 3:
        pc8c_out = sys.argv[2]
    else:
        base, _ = os.path.splitext(png_in)
        if base.endswith(".p8"):
            base = base[:-3]
        pc8c_out = base + ".pc8c"

    if not os.path.exists(png_in):
        print(f"[ERROR] 入力ファイルが見つかりません: {png_in}")
        sys.exit(1)

    success = convert_png_to_pc8c(png_in, pc8c_out)
    if not success:
        sys.exit(1)
