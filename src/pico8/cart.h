// pico8/cart.h — PICO-8 cartridge loader
// Adapted from zepto8 by Sam Hocevar (WTFPL)

#pragma once

#include <vector>
#include <string>

#include "pico8/pico8.h"
#include "pico8/memory.h"

namespace z8::pico8
{

class cart
{
public:
// ⭕ std::string のコピーを一切発生させず、中身がある時だけ安全にキャストして返す
    std::string get_code() const { 
        if (get_code_len() <= 0) return "";
        return std::string(get_code_buf(), get_code_len()); 
    }
    
    // ⭕ 参照戻しをやめ、実体を一時的に返す、または最小限の処理にする
    std::string get_mutable_code() {
        if (get_code_len() <= 0) return "";
        return std::string(get_code_buf(), get_code_len()); 
    }

    bool has_includes() const { return false; }
    void set_has_includes(bool val) { (void)val; }

    cart() {}
    ~cart();

    bool load(std::string const &filename);
    bool load_p8_mem(const char* data, size_t size);
    
    bool load_from_partition(std::string const &filename = "");
    bool save_to_partition() const;

    memory const &get_rom() const { return m_rom; }
    memory       &get_rom()       { return m_rom; }

    std::vector<uint8_t> &get_label() { return m_label; }

    // ⭕ 静的バッファを安全に参照するインライン関数群
    const char* get_code_buf() const { return s_code_buf ? s_code_buf : ""; }
    int get_code_len()   const { return s_code_len; }
    
    // reset_code_buf は cart.cpp 側で実体をゼロクリアします
    void reset_code_buf();

    void append_code_char(char c)
    {
        if (s_code_buf && s_code_len < CODE_BUF_SIZE - 1)
            s_code_buf[s_code_len++] = c;
    }

    void clear_code();
    void init_filename(std::string filename);

    std::string const &get_filename() const { return m_filename; }
    std::string const &get_title()    const { return m_title; }
    std::string const &get_author()   const { return m_author; }

    bool has_file_changed() const { return false; }

    std::vector<uint8_t> get_compressed_code() const;
    std::vector<uint8_t> get_bin() const;
    bool save(std::string const &filename) const;
    void set_from_ram(memory const &ram, int in_dst, int in_src, int in_size);

    std::string preprocess_code() const;

private:
    bool load_png(std::string const &filename);
    bool load_p8(std::string const &filename);
    bool load_lua(std::string const &filename);

    bool save_p8(std::string const &filename) const;
    bool save_png(std::string const &filename) const;

    void set_bin(std::vector<uint8_t> const &data);
    void init_rom();
    void init_title();

    memory               m_rom;
    std::vector<uint8_t> m_label;
    std::string          m_lua;          
    std::string          m_filename;
    std::string          m_title;
    std::string          m_author;
    int                  m_version = 0;

    // ⭕ コンパイラに実体を認識させるためのクラス内宣言（ここを残すのが正解でした！）
    static const int CODE_BUF_SIZE = 65536;  
    static char* s_code_buf;
    static int   s_code_len;
};

} // namespace z8::pico8