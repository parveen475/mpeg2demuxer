
#ifndef TEST_DEMUX_H
#define TEST_DEMUX_H

#include <fcntl.h>

#include "tsDemuxer.h"


#define AV_BUFFER_SIZE          131072
#define POSMAP_PTS_INTERVAL     270000LL

class Demux : public TSDemux::TSDemuxer
{
public:
  Demux(FILE* file, uint16_t channel);
  ~Demux();

  int Do(void);

  const unsigned char* ReadAV(uint64_t pos, size_t n);

private:
  FILE* m_ifile;
  uint16_t m_channel;
  std::map<uint16_t, FILE*> m_outfiles;

  bool get_stream_data(TSDemux::STREAM_PKT* pkt);
  void reset_posmap();
  void register_pmt();
  void show_stream_info(uint16_t pid);
  void write_stream_data(TSDemux::STREAM_PKT* pkt);

  // AV raw buffer
  size_t m_av_buf_size;         ///< size of av buffer
  uint64_t m_av_pos;            ///< absolute position in av
  unsigned char* m_av_buf;      ///< buffer
  unsigned char* m_av_rbs;      ///< raw data start in buffer
  unsigned char* m_av_rbe;      ///< raw data end in buffer

  // Playback context
  TSDemux::AVContext* m_AVContext;
  uint16_t m_mainStreamPID;     ///< PID of main stream
  uint64_t m_DTS;               ///< absolute decode time of main stream
  uint64_t m_PTS;               ///< absolute presentation time of main stream
  int64_t m_pinTime;            ///< pinned relative position (90Khz)
  int64_t m_curTime;            ///< current relative position (90Khz)
  int64_t m_endTime;            ///< last relative marked position (90Khz))
  typedef struct
  {
    uint64_t av_pts;
    uint64_t av_pos;
  } AV_POSMAP_ITEM;
  std::map<int64_t, AV_POSMAP_ITEM> m_posmap;
};

#endif /* TEST_DEMUX_H */

