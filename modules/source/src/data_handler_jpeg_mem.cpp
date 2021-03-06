/*************************************************************************
:a
 * Copyright (C) [2020] by Cambricon, Inc. All rights reserved
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *************************************************************************/
#include "data_handler_jpeg_mem.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <utility>

#include "perf_manager.hpp"

namespace cnstream {

std::shared_ptr<SourceHandler> ESJpegMemHandler::Create(DataSource *module, const std::string &stream_id, int max_width,
                                                        int max_height) {
  if (!module || stream_id.empty()) {
    LOG(ERROR) << "source module or stream id must not be empty";
    return nullptr;
  }
  std::shared_ptr<ESJpegMemHandler> handler(new (std::nothrow)
                                                ESJpegMemHandler(module, stream_id, max_width, max_height));
  return handler;
}

ESJpegMemHandler::ESJpegMemHandler(DataSource *module, const std::string &stream_id, int max_width, int max_height)
    : SourceHandler(module, stream_id) {
  impl_ = new (std::nothrow) ESJpegMemHandlerImpl(module, *this, max_width, max_height);
}

ESJpegMemHandler::~ESJpegMemHandler() {
  if (impl_) {
    delete impl_, impl_ = nullptr;
  }
}

bool ESJpegMemHandler::Open() {
  if (!this->module_) {
    LOG(ERROR) << "module_ null";
    return false;
  }
  if (!impl_) {
    LOG(ERROR) << "impl_ null";
    return false;
  }

  if (stream_index_ == cnstream::INVALID_STREAM_IDX) {
    LOG(ERROR) << "invalid stream_idx";
    return false;
  }

  return impl_->Open();
}

void ESJpegMemHandler::Close() {
  if (impl_) {
    impl_->Close();
  }
}

int ESJpegMemHandler::Write(ESPacket *pkt) {
  if (impl_) {
    return impl_->Write(pkt);
  }
  return -1;
}

bool ESJpegMemHandlerImpl::Open() {
  // updated with paramSet
  DataSource *source = dynamic_cast<DataSource *>(module_);
  if (nullptr != source) {
    param_ = source->GetSourceParam();
  } else {
    LOG(ERROR) << "source module is null";
    return false;
  }

  this->interval_ = param_.interval_;
  SetPerfManager(source->GetPerfManager(stream_id_));
  SetThreadName(module_->GetName(), handler_.GetStreamUniqueIdx());

  size_t MaxSize = 60;  // FIXME
  queue_ = new (std::nothrow) cnstream::BoundedQueue<std::shared_ptr<EsPacket>>(MaxSize);
  if (!queue_) {
    return false;
  }

  // start demuxer
  running_.store(1);
  thread_ = std::thread(&ESJpegMemHandlerImpl::DecodeLoop, this);
  return true;
}

void ESJpegMemHandlerImpl::Close() {
  if (running_.load()) {
    running_.store(0);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  std::lock_guard<std::mutex> lk(queue_mutex_);
  if (queue_) {
    delete queue_;
    queue_ = nullptr;
  }

  if (parser_) {
    parser_->Free();
    delete parser_;
    parser_ = nullptr;
  }
}

int ESJpegMemHandlerImpl::Write(ESPacket *pkt) {
  if (pkt && pkt->data && pkt->size && parser_) {
    if (parser_->Parse(pkt->data, pkt->size) < 0) {
      return -2;
    }
  }
  std::lock_guard<std::mutex> lk(queue_mutex_);
  int timeoutMs = 1000;
  while (running_.load() && queue_) {
    if (queue_->Push(timeoutMs, std::make_shared<EsPacket>(pkt))) {
      return 0;
    }
  }
  return -1;
}

void ESJpegMemHandlerImpl::DecodeLoop() {
  /*meet cnrt requirement*/
  if (param_.device_id_ >= 0) {
    try {
      edk::MluContext mlu_ctx;
      mlu_ctx.SetDeviceId(param_.device_id_);
      // mlu_ctx.SetChannelId(dev_ctx_.ddr_channel);
      mlu_ctx.ConfigureForThisThread();
    } catch (edk::Exception &e) {
      if (nullptr != module_) module_->PostEvent(EVENT_ERROR, "stream_id " + stream_id_ + " failed to setup mlu dev.");
      return;
    }
  }

  if (!PrepareResources()) {
    ClearResources();
    if (nullptr != module_) {
      Event e;
      e.type = EventType::EVENT_STREAM_ERROR;
      e.module_name = module_->GetName();
      e.message = "Prepare codec resources failed.";
      e.stream_id = stream_id_;
      e.thread_id = std::this_thread::get_id();
      module_->PostEvent(e);
    }
    return;
  }

  while (running_.load()) {
    if (!Process()) {
      break;
    }
  }

  ClearResources();
  LOG(INFO) << "DecodeLoop Exit";
}

bool ESJpegMemHandlerImpl::PrepareResources() {
  VideoStreamInfo info;
  if (!running_.load()) {
    return false;
  }

  if (param_.decoder_type_ == DecoderType::DECODER_MLU) {
    info.codec_id = AV_CODEC_ID_MJPEG;
    info.codec_width = max_width_;
    info.codec_height = max_height_;
    decoder_ = std::make_shared<MluDecoder>(this);
  } else if (param_.decoder_type_ == DecoderType::DECODER_CPU) {
    if (parser_) {
      parser_->Free();
      delete parser_;
      parser_ = nullptr;
    }
    parser_ = new (std::nothrow) ParserHelper;
    parser_->Init("mjpeg");

    while (running_.load()) {
      if (parser_->GetInfo(info)) {
        break;
      }
      usleep(1000 * 10);
    }

    decoder_ = std::make_shared<FFmpegCpuDecoder>(this);
  } else {
    LOG(ERROR) << "unsupported decoder_type";
    return false;
  }

  if (!decoder_) {
    return false;
  }

  bool ret = decoder_->Create(&info, interval_);
  if (!ret) {
    return false;
  }

  return true;
}

void ESJpegMemHandlerImpl::ClearResources() {
  if (decoder_.get()) {
    decoder_->Destroy();
  }
}

bool ESJpegMemHandlerImpl::Process() {
  using EsPacketPtr = std::shared_ptr<EsPacket>;

  EsPacketPtr in;
  int timeoutMs = 1000;
  bool ret = this->queue_->Pop(timeoutMs, in);

  if (!ret) {
    // continue.. not exit
    return true;
  }

  if (in->pkt_.flags & ESPacket::FLAG_EOS) {
    ESPacket pkt;
    pkt.data = in->pkt_.data;
    pkt.size = in->pkt_.size;
    pkt.pts = in->pkt_.pts;
    pkt.flags = ESPacket::FLAG_EOS;
    LOG(INFO) << "Stream id: " << stream_id_ << " Eos reached: data ptr, size, pts: "
              << (size_t)pkt.data << ", " << (size_t)pkt.size << ", " << (size_t)pkt.pts;
    decoder_->Process(&pkt);
    return false;
  }  // if (!ret)

  ESPacket pkt;
  pkt.data = in->pkt_.data;
  pkt.size = in->pkt_.size;
  pkt.pts = in->pkt_.pts;
  pkt.flags &= ~ESPacket::FLAG_EOS;

  RecordStartTime(module_->GetName(), pkt.pts);

  if (!decoder_->Process(&pkt)) {
    return false;
  }
  return true;
}

}  // namespace cnstream
