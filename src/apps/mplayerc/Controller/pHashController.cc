
#include "stdafx.h"
#include <sinet.h>
#include "../../../filters/switcher/audioswitcher/AudioSwitcher.h"
#include "pHashController.h"
#include "../Model/PHashComm.h"
#include "HashController.h"
#include "samplerate.h"

#include "sndfile.h"
#include "phashapi.h"
#include "zmq/zmqhelper.h"
#include "Strings.h"

pHashController::pHashController(void)
{
  PHashCommCfg.stop = TRUE;
  PHashCommCfg.index = -1;
  
  PHashCommCfg.stime = 0;
  PHashCommCfg.etime = 0;

  // collect times
  PHashCommCfg.cfg.push_back(1);
  // collect duration (secs)
  PHashCommCfg.cfg.push_back(60);
  // delimiter (don't modify)
  PHashCommCfg.cfg.push_back(0);
  // collect point of time
  PHashCommCfg.cfg.push_back(60);
  PHashCommCfg.cfg.push_back(20);
  PHashCommCfg.cfg.push_back(40);

  PHashCommCfg.data = NULL;

  m_data.clear();
  m_datarefs = 0;
}

pHashController::~pHashController(void)
{

}

void pHashController::_Thread()
{
  m_sphash = HashController::GetInstance()->GetSPHash(m_file.GetBuffer());
  m_file.ReleaseBuffer();

  sinet::refptr<sinet::pool>    net_pool = sinet::pool::create_instance();
  sinet::refptr<sinet::task>    net_task = sinet::task::create_instance();
  sinet::refptr<sinet::request> net_rqst = sinet::request::create_instance();
  
  wchar_t url[512];
  wsprintf(url, L"http://webpj:8080/misc/phash.php?req=%d&sphs=%s\n", 1, m_sphash.c_str());

  net_rqst->set_request_url(url);
  net_rqst->set_request_method(REQ_GET);
  net_task->append_request(net_rqst);
  net_pool->execute(net_task);
    
  while (net_pool->is_running_or_queued(net_task))
  {
    if ( _Exit_state(500))
      return;
  }
  
  //error code dealing
  if (net_rqst->get_response_errcode() != 0)
    return;

  // response data
  std::vector<unsigned char> st_buffer = net_rqst->get_response_buffer();
  st_buffer.push_back(0);
  std::string ret =  (char*)&st_buffer[0];
  ret.push_back(0);

  if (atoi(ret.c_str()) == 1)
  {
    PHashCommCfg.stop = FALSE;
    NewData();
  }

}

void pHashController::UnRefs()
{
  if (--m_datarefs <= 0)
  {
    m_data.clear();
    m_data.resize(0);
  }
}

void pHashController::NewData()
{
  if (PHashCommCfg.index >= PHashCommCfg.cfg.front()-1)
  {
    PHashCommCfg.stop = TRUE;
    PHashCommCfg.index = -1;
    PHashHandler* handler = new PHashHandler(PHashCommCfg.data, m_sphash);
    handler->_Start();
    PHashCommCfg.data = NULL;
    return;
  }

  if (PHashCommCfg.index >= 0)
  {
    PHashHandler* handler = new PHashHandler(PHashCommCfg.data, m_sphash);
    handler->_Start();
  }

  std::vector<BYTE> data;
  m_data.push_back(data);

  PHashCommCfg.data = &(m_data.back());
  PHashCommCfg.index++;

  int pos = 3 + PHashCommCfg.index;
  REFERENCE_TIME durtime = (10000000i64) * PHashCommCfg.cfg[1];

  PHashCommCfg.stime = (10000000i64) * PHashCommCfg.cfg[pos];
  PHashCommCfg.etime = PHashCommCfg.stime + durtime;

  m_datarefs++;
}

void pHashController::Check(REFERENCE_TIME& time, CComQIPtr<IMediaSeeking> ms,
                            CComQIPtr<IAudioSwitcherFilter> pASF, CString file)
{
  PHashCommCfg.stop = TRUE;

  if (PHashCommCfg.data)
  { // reset
    PHashCommCfg.data->clear();
    PHashCommCfg.data->resize(0);
    PHashCommCfg.data = NULL;
    PHashCommCfg.index = -1;
  }

  if (time != 0)
    return;

  if (!ms)
    return;

  __int64 totaltime = 0;
  ms->GetDuration(&totaltime);
  if (totaltime < (10000000i64)*60*45)
    return;

  m_file = file;
  pASF->SetPhashCfg(&PHashCommCfg);

  _Stop();
  _Start();
}

PHashHandler::PHashHandler(std::vector<BYTE>* data, std::wstring sphash)
{
  m_sr = 8000;

  m_data = data;
  m_sphash = sphash;

  m_phash = NULL;
  m_phashlen = 0;
}

PHashHandler::~PHashHandler(void)
{

}

void PHashHandler::_Thread()
{
  if (!SamplesToPhash())
    return;

  if (!m_phashlen || !m_phash)
    return;

  void* context = zmq_init(1);
  if (!context)
  {
    ph_freemem_hash(NULL, m_phash);
    return;
  }

  void* client = socket_connect(context, ZMQ_REQ, "tcp://192.168.10.29:5000");
  if (!client)
  {
    ph_freemem_hash(NULL, m_phash);
    return;
  }

  std::string sphash = Strings::WStringToString(m_sphash);
  int sphashlen = sphash.size();
  sendmore_msg_data(client, &sphashlen, sizeof(int), NULL, NULL);
  sendmore_msg_data(client, &sphash[0], sphashlen, NULL, NULL);
  sendmore_msg_data(client, &m_phashlen, sizeof(int), NULL, NULL);
  send_msg_data(client, m_phash, m_phashlen*sizeof(uint32_t), NULL, NULL);

  void* data;
  size_t msgsize, moresize;
  int64_t more;
  recieve_msg(client, &msgsize, &more, &moresize, (void**)&data);
  
  ph_freemem_hash(NULL, m_phash);
  m_phash = NULL;
  m_phashlen = 0;

  zmq_close(client);
  zmq_term(context);
}

BOOL PHashHandler::SamplesToPhash()
{
  long datalen = m_data->size();
  if (!datalen)
    return FALSE;

  pHashController* phashctrl = pHashController::GetInstance();
  WORD channels = phashctrl->PHashCommCfg.format.nChannels;
  if (channels != 2 && channels != 6)
    return FALSE;

  int samplebyte = (phashctrl->PHashCommCfg.format.wBitsPerSample >> 3);
  int nsample = datalen / channels / samplebyte;
  int samples = nsample * channels;
  
  BYTE* indata;
  BOOL ret;

  float* buf = new float[samples];
  // Making all data into float type
  indata = &(*m_data)[0];
  ret = SampleToFloat(indata, buf, samples, phashctrl->PHashCommCfg.pcmtype);

  m_data->clear();
  m_data->resize(0);
  pHashController::GetInstance()->UnRefs();

  if (!ret)
  {
    delete [] buf;
    return ret;
  }

  // Mix as Mono channel
  float* MonoChannelBuf;
  ret = MixChannels(buf, samples, phashctrl->PHashCommCfg.format.nChannels, nsample, &MonoChannelBuf);
  delete[] buf;

  if (!ret)
  {
    delete[] MonoChannelBuf;
    return ret;
  }

  // Samplerate to 8kHz 
  float* outbuff = NULL;
  int bufflen = 0;
  ret = DownSample(MonoChannelBuf, nsample, m_sr, phashctrl->PHashCommCfg.format.nSamplesPerSec, &outbuff, bufflen);
  delete[] MonoChannelBuf;

  if (!ret)
    return ret;

  int phashlen = 0;
  uint32_t* phash = NULL;

  m_phash = ph_audiohash(outbuff, bufflen, m_sr, m_phashlen);
  delete [] outbuff;

  if (!m_phash)
    m_phashlen = 0;

  return ret;
}

BOOL PHashHandler::DownSample(float* inbuf, int nsample, int des_sr, int org_sr, float** outbuf, int& outlen)
{
  // resample float array , set desired samplerate ratio
  double sr_ratio = (double)des_sr / (double)org_sr;
  if (!src_is_valid_ratio(sr_ratio))
    return FALSE;

  // allocate output buffer for conversion
  outlen = sr_ratio * (double)nsample;

  float* buffer = new float[outlen * sizeof(float)];
  if (!buffer)
    return FALSE;

  int error;
  SRC_STATE *src_state = src_new(SRC_LINEAR, 1, &error);
  if (!src_state)
    return FALSE;

  SRC_DATA src_data;
  src_data.data_in = inbuf;
  src_data.data_out = buffer;
  src_data.input_frames = nsample;
  src_data.output_frames = outlen;
  src_data.end_of_input = SF_TRUE;
  src_data.src_ratio = sr_ratio;

  // Sample rate conversion
  if (src_process(src_state, &src_data))
  {
    delete [] buffer;
    src_delete(src_state);
    return FALSE;
  }

  src_delete(src_state);
  *outbuf = buffer;
  return TRUE;
}

BOOL PHashHandler::MixChannels(float* buf, int samples, int channels, int nsample, float** MonoChannelBuf)
{
  float* monobuffer = new float[nsample];
  if (channels == 2)
  {
    int bufindx = 0;
    for (int j = 0; j < samples; j += channels)
    {
      monobuffer[bufindx] = 0.0f;
      for (int i = 0; i < channels; ++i)
        monobuffer[bufindx] += buf[j+i];
      monobuffer[bufindx++] /= channels;
    }
    *MonoChannelBuf = monobuffer;
  }
  else if (channels == 6)
  {
    int bufindx = 0;

    for (int i = 0; i < samples; i += channels)
    {
      monobuffer[bufindx] = (buf[i] + buf[i+1] + buf[i+3] + buf[i+5]) / 3.f;
      monobuffer[bufindx] += (buf[i+1] + buf[i+2] + buf[i+4] + buf[i+5]) / 3.f;
      monobuffer[bufindx++] /= 2.f;
    }
      
    *MonoChannelBuf = monobuffer;
  }

  return TRUE;
}

BOOL PHashHandler::SampleToFloat(const unsigned char* const indata, float* outdata, int samples, int type)
{
  int samplecnt = 0;
  int tmp;

  switch (type)
  {
   // PCM8
  case CAudioSwitcherFilter::WETYPE_PCM8:
    for (samplecnt = 0; samplecnt < samples; samplecnt++)      
      outdata[samplecnt] = ((float)(((BYTE*)indata)[samplecnt]) -0x7f) / 0x80; //UCHAR_MAX
    break;
   // PCM16
  case CAudioSwitcherFilter::WETYPE_PCM16:
    for (samplecnt = 0; samplecnt < samples; samplecnt++)
      outdata[samplecnt] = (float)(((short*)indata)[samplecnt])/(float)SHRT_MAX;
    break;  
   // PCM24
  case CAudioSwitcherFilter::WETYPE_PCM24:
    for (samplecnt = 0; samplecnt < samples; samplecnt++)
    {
      memcpy(((BYTE*)&tmp)+1, &indata[3*samplecnt], 3);  
      outdata[samplecnt] += (float)(tmp >> 8) / ((1<<23)-1);
    }
    break;
   // PCM32
  case CAudioSwitcherFilter::WETYPE_PCM32:
    for (samplecnt = 0; samplecnt < samples; samplecnt++)
      outdata[samplecnt] = (float)(((int*)indata)[samplecnt]);
    break;
    // FPCM32
  case CAudioSwitcherFilter::WETYPE_FPCM32:
    for (samplecnt = 0; samplecnt < samples; samplecnt++)
      outdata[samplecnt] = (float)(((float*)indata)[samplecnt]);
    break;
    // FPCM64: did not have it tested
  case CAudioSwitcherFilter::WETYPE_FPCM64: 
    for (samplecnt = 0; samplecnt < samples; samplecnt++)
      outdata[samplecnt] = (float)(((double*)indata)[samplecnt]);
    break;

  default:
    return FALSE;
  }

  return TRUE;
}