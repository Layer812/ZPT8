// textfile.h — Save-file management with write-rate limiting
// Adapted from zepto8 by Sam Hocevar (WTFPL)
// Changes: file I/O uses lol_compat (SD card on Arduino)

#pragma once

#include "zepto8.h"

namespace z8
{

class textfile
{
public:
    textfile() = default;

    bool tick(bool force);
    bool read_save(std::string filepath, uint8_t *data);
    bool write_save(std::string filepath, uint8_t *data);
    bool read_config(std::string filepath, uint8_t *data);
    bool write_config(std::string filepath, uint8_t *data);

    void set_dirty() { m_is_dirty = true; }

private:
    bool m_is_dirty = false;
    int  m_min_frames_between_saves = 640;
    int  m_frames_since_last_save   = 0;
};

} // namespace z8
