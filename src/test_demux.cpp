#define __STDC_FORMAT_MACROS 1
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string>
#include <inttypes.h>

#include "test_demux.h"

int g_parseonly = 0;

Demux::Demux(FILE* file, uint16_t channel)
: m_channel(channel)
{
  m_ifile = file;
  m_av_buf_size = AV_BUFFER_SIZE;
  m_av_buf = (unsigned char*)malloc(sizeof(*m_av_buf) * (m_av_buf_size + 1));
  if (m_av_buf)
  {
    m_av_pos = 0;
    m_av_rbs = m_av_buf;
    m_av_rbe = m_av_buf;
    m_channel = channel;

    
    m_mainStreamPID = 0xffff;
    m_DTS = PTS_UNSET;
    m_PTS = PTS_UNSET;
    m_pinTime = m_curTime = m_endTime = 0;
    m_AVContext = new TSDemux::AVContext(this, 0, m_channel);
  }
}

Demux::~Demux()
{
  for (std::map<uint16_t, FILE*>::iterator it = m_outfiles.begin(); it != m_outfiles.end(); ++it)
    if (it->second)
      fclose(it->second);

  // Free AV context
  if (m_AVContext)
    delete m_AVContext;
  // Free AV buffer
  if (m_av_buf)
  {
    free(m_av_buf);
    m_av_buf = NULL;
  }
}

const unsigned char* Demux::ReadAV(uint64_t pos, size_t n)
{
  // out of range
  if (n > m_av_buf_size)
    return NULL;

  // Already read ?
  size_t sz = m_av_rbe - m_av_buf;
  if (pos < m_av_pos || pos > (m_av_pos + sz))
  {
    // seek and reset buffer

    int ret = fseek(m_ifile, (int64_t)pos, SEEK_SET);
    if (ret != 0)
      return NULL;
    m_av_pos = (uint64_t)pos;
    m_av_rbs = m_av_rbe = m_av_buf;
  }
  else
  {
    // move to the desired pos in buffer
    m_av_rbs = m_av_buf + (size_t)(pos - m_av_pos);
  }

  size_t dataread = m_av_rbe - m_av_rbs;
  if (dataread >= n)
    return m_av_rbs;

  memmove(m_av_buf, m_av_rbs, dataread);
  m_av_rbs = m_av_buf;
  m_av_rbe = m_av_rbs + dataread;
  m_av_pos = pos;
  unsigned int len = (unsigned int)(m_av_buf_size - dataread);

  while (len > 0)
  {
    size_t c = fread(m_av_rbe, sizeof(*m_av_buf), len, m_ifile);
    if (c > 0)
    {
      m_av_rbe += c;
      dataread += c;
      len -= c;
    }
    if (dataread >= n || c == 0)
      break;
  }
  return dataread >= n ? m_av_rbs : NULL;
}

int Demux::Do()
{
  int ret = 0;

  while (true)
  {
    {
      ret = m_AVContext->TSResync();
    }
    if (ret != TSDemux::AVCONTEXT_CONTINUE)
      break;

    ret = m_AVContext->ProcessTSPacket();

    if (m_AVContext->HasPIDStreamData())
    {
      TSDemux::STREAM_PKT pkt;
      while (get_stream_data(&pkt))
      {
        if (pkt.streamChange)
          show_stream_info(pkt.pid);
        write_stream_data(&pkt);
      }
    }
    if (m_AVContext->HasPIDPayload())
    {
      ret = m_AVContext->ProcessTSPayload();
      if (ret == TSDemux::AVCONTEXT_PROGRAM_CHANGE)
      {
        register_pmt();
        std::vector<TSDemux::ElementaryStream*> streams = m_AVContext->GetStreams();
        for (std::vector<TSDemux::ElementaryStream*>::const_iterator it = streams.begin(); it != streams.end(); ++it)
        {
          if ((*it)->has_stream_info)
            show_stream_info((*it)->pid);
        }
      }
    }

    if (ret == TSDemux::AVCONTEXT_TS_ERROR)
      m_AVContext->Shift();
    else
      m_AVContext->GoNext();
  }

  
  return ret;
}

bool Demux::get_stream_data(TSDemux::STREAM_PKT* pkt)
{
  TSDemux::ElementaryStream* es = m_AVContext->GetPIDStream();
  if (!es)
    return false;

  if (!es->GetStreamPacket(pkt))
    return false;

  if (pkt->duration > 180000)
  {
    pkt->duration = 0;
  }
  else if (pkt->pid == m_mainStreamPID)
  {
    // Fill duration map for main stream
    m_curTime += pkt->duration;
    if (m_curTime >= m_pinTime)
    {
      m_pinTime += POSMAP_PTS_INTERVAL;
      if (m_curTime > m_endTime)
      {
        AV_POSMAP_ITEM item;
        item.av_pts = pkt->pts;
        item.av_pos = m_AVContext->GetPosition();
        m_posmap.insert(std::make_pair(m_curTime, item));
        m_endTime = m_curTime;
       }
    }
    // Sync main DTS & PTS
    m_DTS = pkt->dts;
    m_PTS = pkt->pts;
  }
  return true;
}

void Demux::reset_posmap()
{
  if (m_posmap.empty())
    return;

  {
    m_posmap.clear();
    m_pinTime = m_curTime = m_endTime = 0;
  }
}

void Demux::register_pmt()
{
  const std::vector<TSDemux::ElementaryStream*> es_streams = m_AVContext->GetStreams();
  if (!es_streams.empty())
  {
    m_mainStreamPID = es_streams[0]->pid;
    for (std::vector<TSDemux::ElementaryStream*>::const_iterator it = es_streams.begin(); it != es_streams.end(); ++it)
    {
      uint16_t channel = m_AVContext->GetChannel((*it)->pid);
      const char* codec_name = (*it)->GetStreamCodecName();
      m_AVContext->StartStreaming((*it)->pid);
    }
  }
}


static inline int stream_identifier(int composition_id, int ancillary_id)
{
  return ((composition_id & 0xff00) >> 8)
    | ((composition_id & 0xff) << 8)
    | ((ancillary_id & 0xff00) << 16)
    | ((ancillary_id & 0xff) << 24);
}

void Demux::show_stream_info(uint16_t pid)
{
  TSDemux::ElementaryStream* es = m_AVContext->GetStream(pid);
  if (!es)
    return;

 }

void Demux::write_stream_data(TSDemux::STREAM_PKT* pkt)
{
  if (!pkt)
    return;

  if (!g_parseonly && pkt->size > 0 && pkt->data)
  {
    std::map<uint16_t, FILE*>::const_iterator it = m_outfiles.find(pkt->pid);
    if (it != m_outfiles.end())
    {
      size_t c = fwrite(pkt->data, sizeof(*pkt->data), pkt->size, it->second);
      if (c != pkt->size)
        m_AVContext->StopStreaming(pkt->pid);
    }
  }
}

static void usage(const char* cmd)
{
  fprintf(stderr,
        "Usage: %s [options] <file> | -\n\n"
        "  Enter '-' instead a file name will process stream from standard input\n\n"
        "  --debug            enable debug output\n"
        "  --parseonly        only parse streams\n"
        "  --channel <id>     process channel <id>. Default 0 for all channels\n"
        "  -h, --help         print this help\n"
        "\n", cmd
        );
}

int main(int argc, char* argv[])
{
  const char* filename = NULL;
  uint16_t channel = 0;
  int i = 0;
  while (++i < argc)
  {
    filename = argv[i];
  }

  if (filename)
  {
    FILE* file = NULL;
    if (strcmp(filename, "-") == 0)
    {
      file = stdin;
      filename = "[stdin]";
    }
    else
      file = fopen(filename, "rb");

    if (file)
    {
      fprintf(stderr, "## Processing TS stream from %s ##\n", filename);
      Demux* demux = new Demux(file, channel);
      demux->Do();
      delete demux;
      fclose(file);
    }
    else
    {
      fprintf(stderr,"Cannot open file '%s' for read\n", filename);
      return -1;
    }
  }
  else
  {
    fprintf(stderr, "No file specified\n\n");
    usage(argv[0]);
    return -1;
  }
  return 0;
}
