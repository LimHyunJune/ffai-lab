#include "Controller.h"
#include "Timer.h"
#include <cmath>

Controller::Controller(){}

Controller::~Controller()
{
    delete input_handler;
    delete decoder_handler;
    delete filter_handler;
    delete encoder_handler;
    delete output_handler;

    av_buffer_unref(&hw_frames_ctx);
    av_buffer_unref(&hw_device_ctx);

}

void Controller::start()
{
    controller_config.main_input_path = "srt://0.0.0.0:8080?mode=listener&latency=2000&pkt_size=1316&rcvbuf=1073741824";
    controller_config.backup_input_path = "srt://0.0.0.0:8081?mode=listener&latency=2000&pkt_size=1316&rcvbuf=1073741824";
    // controller_config.input_path = "/playout-multiview-generator/assets/jungkook.mp4";

    controller_config.filter_config.filter_type = FilterType::MULTIVIEW;
    controller_config.filter_config.use_gpu = true;
    controller_config.filter_config.frame_rate = AVRational{60,1};
    controller_config.filter_config.time_base = AVRational{1,90000};
    controller_config.filter_config.width = 3840;
    controller_config.filter_config.height = 2160;

    controller_config.encoder_config.abr = false;
    vector<EncoderParam> encoder_params;
    EncoderParam resolution_1 = {3840,2160,12500000};
    EncoderParam resolution_2 = {2560,1440,6500000};
    EncoderParam resolution_3 = {1920,1080,3000000};
    EncoderParam resolution_4 = {1280,720,1500000};
    encoder_params.push_back(resolution_1);
    encoder_params.push_back(resolution_2);
    encoder_params.push_back(resolution_3);
    encoder_params.push_back(resolution_4);
    controller_config.encoder_config.params = encoder_params;
    controller_config.encoder_config.frame_rate = AVRational{60,1};
    controller_config.encoder_config.time_base = AVRational{1,90000};
    controller_config.encoder_config.use_gpu = true;
    controller_config.encoder_config.preset = "p4";
    controller_config.encoder_config.encoder_type = EncoderType::H265;

    controller_config.output_config.output_type = OutputType::SRT;
    vector<OutputParam> output_params;
    OutputParam addr_1 = {nullptr, "./out1.mp4"};
    OutputParam addr_2 = {nullptr, "./out2.mp4"};
    OutputParam addr_3 = {nullptr, "./out3.mp4"};
    OutputParam addr_4 = {nullptr, "./out4.mp4"};
    output_params.push_back(addr_1);
    output_params.push_back(addr_2);
    output_params.push_back(addr_3);
    output_params.push_back(addr_4);
    controller_config.output_config.params = output_params;

    create_streaming_pipeline();
}

bool Controller::init_hw_device()
{
    Timer init_time_check;
    int ret =  av_hwdevice_ctx_create(&hw_device_ctx, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
    if (ret < 0)
    {
        Logger::get_instance().print_log_with_reason(AV_LOG_ERROR,"error init hw device",ret);
        return false;
    }

    hw_frames_ctx = av_hwframe_ctx_alloc(hw_device_ctx);
    if(!hw_frames_ctx) return false;

    AVHWFramesContext* fctx = (AVHWFramesContext*)hw_frames_ctx->data;
    fctx->format = AV_PIX_FMT_CUDA;
    fctx->sw_format = AV_PIX_FMT_NV12;
    fctx->width = 3840;
    fctx->height = 2160;
    fctx->initial_pool_size = 8;
    if((ret = av_hwframe_ctx_init(hw_frames_ctx)) < 0)
        return false;
    BOOST_LOG(debug) << "[HW DEVICE] init time : " << init_time_check.elapsed();
    return true;
}

int Controller::interrupt_cb(void* opaque)
{
    auto* self = static_cast<Controller*>(opaque);
    const int64_t now = av_gettime_relative();
    return (now - self->last_io_ts_us.load()) > self->io_timeout_us ? 1 : 0;
}


bool Controller::init_input()
{
    Timer init_time_check;
    input_handler = new InputHandler(controller_config.main_input_path, controller_config.backup_input_path);
    
    main_input_ctx = input_handler->get_main_input_context();
    if(!main_input_ctx)
        return false;
    
    main_video_streams = input_handler->get_main_video_stream();

    // backup_input_ctx = input_handler->get_backup_input_context();
    // backup_video_streams = input_handler->get_backup_video_stream();

    

    // main_input_ctx->interrupt_callback.callback = &Controller::interrupt_cb;
    // main_input_ctx->interrupt_callback.opaque = this;

    // if(backup_input_ctx)
    // {
    //     backup_input_ctx->interrupt_callback.callback = &Controller::interrupt_cb;
    //     backup_input_ctx->interrupt_callback.opaque = this;
    // }

    active_input_ctx = main_input_ctx;
    active_video_streams = main_video_streams;

    // last_io_ts_us.store(av_gettime_relative());

    // main_alive.store(true);
    // backup_alive.store(backup_input_ctx != nullptr);
    BOOST_LOG(debug) << "[INPUT] init time : " << init_time_check.elapsed();
    return true;
}

bool Controller::init_decoder()
{
    Timer init_time_check;

    decoder_handler = new DecoderHandler();
    dec_ctxs = decoder_handler->get_decoder_codec_context(active_video_streams, hw_device_ctx);
    for(auto dec : dec_ctxs)
    {
        av_buffer_unref(&dec.second->hw_frames_ctx);
        dec.second->hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
        dec.second->thread_count = 1;
    }

    BOOST_LOG(debug) << "[DECODER] init time : " << init_time_check.elapsed();

    if(!dec_ctxs.size())
        return false;
    return true;
}

bool Controller::init_filter()
{
    Timer init_time_check;
    controller_config.filter_config.hw_frames_ctx = av_buffer_ref(hw_frames_ctx);
    filter_handler = new FilterHandler(controller_config.filter_config);
    
    pair<map<int,AVFilterContext*>, AVFilterContext*> buffer = filter_handler->get_filter_context();
    buffersrc_ctxs = buffer.first;
    buffersink_ctx = buffer.second;

    BOOST_LOG(debug) << "[FILTER] init time : " << init_time_check.elapsed();
    if(!buffersrc_ctxs.size() || !buffersink_ctx)
        return false;
    return true;
}

bool Controller::init_encoder()
{
    Timer init_time_check;

    encoder_handler = new EncoderHandler(controller_config.encoder_config);
    enc_ctxs = encoder_handler->get_encoder_codec_context(hw_device_ctx);
    BOOST_LOG(debug) << "[ENCODER] init time : " << init_time_check.elapsed();

    if(!enc_ctxs.size())
        return false;
    return true;
}

bool Controller::init_output()
{
    Timer init_time_check;

    vector<OutputParam> params;
    for(int i = 0; i < enc_ctxs.size(); i++)
        controller_config.output_config.params[i].enc_ctx = enc_ctxs[i];

    output_handler = new OutputHandler(controller_config.output_config);
    out_fmt_ctxs = output_handler->get_output_format_context();
    output_streams = output_handler->get_output_stream();
    BOOST_LOG(debug) << "[OUTPUT] init time : " << init_time_check.elapsed();

    if(!out_fmt_ctxs.size())
        return false;
    return true;
}

bool Controller::init_vmaf()
{
    for(int i = 0; i < enc_ctxs.size(); i++)
    {
        vmaf_handler[i] = new VmafHandler();
        if(!(vmaf_handler[i]->init(enc_ctxs[i])))
            return false;
    }
    return true;
}

/*
공통
1) 각 TSQueue 길이, push 실패/대기 시간
2) 스테이지 별 처리시간 히스토그램
3) 파이프라인 end to end 지연 (입력 PTS -> 출력 Mux DTS)
*/


/*
Input 

1) 패킷 유입률 측정 : pkt/s (초당 패킷 수) o
2) 인터 도착 jitter : ts(i) - ts(i-1)의 분산/최대 o
3) 스트림 타임 스탬프 건강도 : 역행 / 정지 감지
4) drop 카운트 o

advanced : SRT 레벨에서 송.수신 버퍼 체크, NAK/retrans 수 

*/

void Controller::run_input_thread() {

    input_thread = std::thread([this]() {
        BOOST_LOG(debug) << "[INPUT] input thread start";

        // check packet inflow (1s)
        Timer check_packet_inflow;
        int pkt_cnt = 0;
        size_t pkt_size = 0;

        // check read interval change
        double max_inter = 0.0;
        Timer check_interval;

        while (flag) {
            AVPacket* pkt = av_packet_alloc();
    
            int ret = av_read_frame(active_input_ctx, pkt);
            if(ret >= 0)
            {
                if(active_input_ctx->streams[pkt->stream_index]->codecpar->codec_type != AVMEDIA_TYPE_VIDEO) 
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    av_packet_free(&pkt);
                    continue;
                }
                
                // check read frame interval
                double cur_inter = round(check_interval.elapsed()*1e7)/1e7;
                if(cur_inter > max_inter)
                {
                    BOOST_LOG(debug) << "[INPUT] packet receive interval increased : " << max_inter;
                    max_inter = cur_inter;
                }    
                check_interval.reset();

                AVPacket* copy = av_packet_alloc();
                av_packet_ref(copy, pkt);
                
                if(check_packet_inflow.elapsed() < 1.0)
                {
                    pkt_cnt++;
                    pkt_size += pkt->size;
                }
                else
                {
                    BOOST_LOG(debug) << "[INPUT] packet inflow rate : " << pkt_cnt << "/s , flow size : " << pkt_size;
                    pkt_cnt = 0;
                    pkt_size = 0;
                    check_packet_inflow.reset();
                }

                input_queue.push(copy);                
                av_packet_free(&pkt);
            }
            else
            {
                // Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "input read error.", ret);
                av_packet_free(&pkt);
            }
        }
        BOOST_LOG(debug) << "[INPUT] input loop exit";
        input_queue.stop();
    });
}

/*
Decoder

1) 디코딩 지연 : send packet -> receive frame 까지 
2) 프레임 처리률 : frame/s
3) 프레임 드롭/리커버리 (에러/reorder 실패 등) 카운트 o
4) 코덱 별 경고/ 재동기화 이벤트
*/


void Controller::run_decoder_thread() {

    decoder_thread = std::thread([this]() {
        BOOST_LOG(debug) << "[DECODER] decoder thread start";
        AVPacket* pkt = nullptr;

        Timer check_frame_inflow;
        int frame_cnt = 0;

        while (flag && input_queue.pop(pkt)) {
            int idx = pkt->stream_index;
            if(!buffersrc_ctxs[idx])
            {
                av_packet_free(&pkt);
                continue;
            }
            int ret = avcodec_send_packet(dec_ctxs[idx], pkt);
            if (ret < 0) {
                Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "[DECODER] decoder send error", ret);
                av_packet_free(&pkt);
                continue;
            }
                
            int drop_cnt = 0;
            int64_t last_pts = AV_NOPTS_VALUE;

            while (true) {
                AVFrame* frame = av_frame_alloc();
                int ret = avcodec_receive_frame(dec_ctxs[idx], frame);
                if (ret >= 0) {

                    if(last_pts != AV_NOPTS_VALUE && frame->pts < last_pts)
                        BOOST_LOG(error) << "[DECODER] frame reorder failed.";
                    last_pts = frame->pts;

                    frame->opaque = (void*)(intptr_t)idx;
                    decoder_queue.push(frame);

                    if(check_frame_inflow.elapsed() < 1.0)
                        frame_cnt++;
                    else
                    {
                        BOOST_LOG(debug) << "[DECODER] frame inflow rate : " << frame_cnt << "/s";
                        frame_cnt = 0;
                        check_frame_inflow.reset();
                    }
                }
                else
                {
                    if(ret == AVERROR_INVALIDDATA)
                    {
                        BOOST_LOG(error) << "[DECODER] frame dropped.";
                        drop_cnt++;
                    }
                    else if(ret != AVERROR(EAGAIN))
                        Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "decoder receive error.", ret);
                    av_frame_free(&frame);
                    break;
                }
            }
            if(drop_cnt)
                BOOST_LOG(debug) << "[DECODER] dropped frame count : " << drop_cnt;
            av_packet_free(&pkt);
        }
        BOOST_LOG(debug) << "[DECODER] decoder loop exit";
        decoder_queue.stop();
    });
}

static inline bool is_hw_frame_fmt(int fmt) { return fmt == AV_PIX_FMT_CUDA; }

static AVPixelFormat pick_swfmt_from_dec(AVCodecContext* dec) {
    if (dec && dec->hw_frames_ctx) {
        auto* fctx = (AVHWFramesContext*)dec->hw_frames_ctx->data;
        if (fctx && fctx->sw_format != AV_PIX_FMT_NONE) return (AVPixelFormat)fctx->sw_format;
    }
    return AV_PIX_FMT_NV12;
}

/*
Filter

1) buffersrc_add_frame 대기/지연 (ms)
2) buffersink_get_frame 루프 당 산출률,지연
3) filter 그래프 지연 : 입력 pts -> 출력 pts jitter
4) frame drop 
5) GPU 사용률 / 메모리 (NVML 연동)

*/

void Controller::run_filter_thread() {

    filter_thread = std::thread([this]() {
        BOOST_LOG(debug) << "[FILTER] filter thread start";
        AVFrame* frame = nullptr;
        while (flag && decoder_queue.pop(frame)) {
            int idx = (int)(intptr_t)frame->opaque;

            int ret = av_buffersrc_add_frame_flags(buffersrc_ctxs[idx], frame, 0);
            if(ret < 0)
            {
                Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "[FILTER] add filter buffer source error.", ret);
            }
            av_frame_free(&frame);

            while (true) {
                AVFrame* filt_frame = av_frame_alloc();
                ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                if (ret >= 0) {
                    filter_queue.push(filt_frame);
                }
                else {
                    if(ret != AVERROR(EAGAIN))
                        Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "[FILTER] filter buffer sink error.", ret);
                    av_frame_free(&filt_frame);
                    break;
                }
            }
        }
        BOOST_LOG(debug) << "[FILTER] filter loop exit";
        filter_queue.stop();
    });
}

/*
Encoder

1) 인코딩 지연 : send frame -> receive pakcet 까지 (ms)
2) 산출 비트레이트
3) frame type 분포 : IDR/I/P/B 카운트
4) QP(평균 or 최대 or 분산) 또는 슬라이스 레벨 QP delta
5) VBV 근사 : 순간 bit burst, 언더런 징후 (프레임 길이 변동성)
6) HDR 메타 : mastering display / content light level (있는 경우) 

*/

void Controller::run_encoder_thread() {

    encoder_thread = std::thread([this]() {
        BOOST_LOG(debug) << "[ENCODER] encoder thread start";
        AVFrame* frame = nullptr;
        TSQueue<AVFrame*>& src_queue = controller_config.filter_config.filter_type != FilterType::NONE ? filter_queue : decoder_queue;
        
        auto score_sum = make_shared<vector<atomic<double>>>(enc_ctxs.size());
        auto score_cnt = make_shared<vector<atomic<int>>>(enc_ctxs.size());
        map<int,Timer*> timers;

        for(int i = 0; i < enc_ctxs.size(); i++)
        {
            timers[i] = new Timer();
            (*score_sum)[i].store(0.0, memory_order_release);
            (*score_cnt)[i].store(0, memory_order_release);
        }


        while (flag && src_queue.pop(frame)) {
            for(int i = 0; i < (int)enc_ctxs.size(); i++)
            {
                AVFrame* in = av_frame_alloc();
                if(av_frame_ref(in, frame) < 0)
                {
                    BOOST_LOG(debug) << "[ENCODER] av_frame_ref frame to in failed";
                    av_frame_free(&in);
                    continue;
                }

                AVFrame* dst = nullptr;
                int er = encoder_handler->scale_frame(i, in, &dst);
                if (er < 0 || !dst) {
                    BOOST_LOG(error) << "[ENCODER] scale_frame failed: " << er
                                    << " (in_fmt=" << in->format
                                    << ", in=" << in->width << "x" << in->height
                                    << ", out=" << enc_ctxs[i]->width << "x" << enc_ctxs[i]->height << ")";
                    av_frame_free(&in);
                    continue;
                }
                av_frame_free(&in);

                AVFrame* ref_clone = vmaf_handler[i]->make_ref_for_vmaf(dst, enc_ctxs[i]->width, enc_ctxs[i]->height);
                if (!ref_clone)
                {
                    av_frame_free(&dst);
                    continue; 
                }
                dst->opaque = ref_clone;

                int sret = avcodec_send_frame(enc_ctxs[i], dst);
                if (sret < 0) {
                    Logger::get_instance().print_log_with_reason(AV_LOG_ERROR, "send_frame failed", sret);
                    // send 실패 시 직접 해제
                    av_frame_free(&ref_clone);
                    av_frame_free(&dst);
                    continue;
                }
    
                while (true) {
                    AVPacket* pkt = av_packet_alloc();
                    int ret = avcodec_receive_packet(enc_ctxs[i], pkt);
                    
                    if (ret >= 0) {
                        AVPacket* copy = av_packet_alloc();
                        av_packet_ref(copy, pkt);
                        
                        // int는 4byte이므로 8byte void*에 그대로 담을 시 4byte garbage 발생
                        // intptr_t는 손실없이 포인터 변환이 가능하므로 승격이 필요하고, static_cast로 정수 영역에서 안전 승격
                        copy->opaque = reinterpret_cast<void*>(static_cast<intptr_t>(i));
                        encoder_queue.push(copy);
    
                        // 1초 주기로 전송 시 VCL NAL unit 실패, 그냥 연달아 전송 시 성공
                        // vmaf handler의 dec ctx에서 참조 체인이 깨져서 실패함
                        // 주기적으로 보내려면 IDR만 전송해야함 
    
                        AVFrame* vmaf_ref_frame = reinterpret_cast<AVFrame*>(pkt->opaque);
                        if(!vmaf_ref_frame)
                        {
                            av_packet_free(&pkt);
                            continue;
                        }

                        if((pkt->flags & AV_PKT_FLAG_KEY) && timers[i]->elapsed() >= 4)
                        {
                            AVPacket* vmaf_ref_pkt = av_packet_alloc();
                            av_packet_ref(vmaf_ref_pkt, pkt);

                            // std::thread([this,vmaf_ref_pkt,vmaf_ref_frame,score_sum,score_cnt,i](){
                                try
                                {
                                    double score = vmaf_handler[i]->get_score(vmaf_ref_frame, vmaf_ref_pkt);
                                    if(score)
                                    {
                                        BOOST_LOG(debug) << "[VMAF] score ("<< i <<") : " << score;
                                        double old_val = (*score_sum)[i].load(std::memory_order_relaxed);
                                        while (!(*score_sum)[i].compare_exchange_weak(
                                                old_val, old_val + score,
                                                std::memory_order_release,
                                                std::memory_order_relaxed)) {}
        
                                        (*score_cnt)[i].fetch_add(1,memory_order_relaxed);
                                    }
                                }
                                catch(const exception& e)
                                {
                                    BOOST_LOG(error) << "[VMAF] exception : " << e.what();
                                }
                                catch(...)
                                {
                                    BOOST_LOG(error) << "[VMAF] unknown exception occurred";
                                }
                                
                            // }).detach();
                            
                            if((*score_cnt)[i].load(memory_order_acquire) >= 10)
                            {
                                int cnt = (*score_cnt)[i].exchange(0, memory_order_acq_rel);
                                double sum = (*score_sum)[i].exchange(0.0, memory_order_acq_rel);
                                double average = (sum / (double)cnt);
                                bool is_ok = (average >= 90) ? true : false;
                                BOOST_LOG(debug) << "[VMAF] average score ("<< i <<") : " << average << " pass (" << is_ok << ")";
                            }
                            timers[i]->reset();
                        }
                        else
                            av_frame_free(&vmaf_ref_frame);
                        av_packet_free(&pkt);
                    }
                    else
                    {
                        av_packet_free(&pkt);
                        break;
                    }
                }
                av_frame_free(&dst);
            }
            av_frame_free(&frame);
        }
        BOOST_LOG(debug) << "encoder loop exit";
        encoder_queue.stop();
    });
}

/*
Output

1) Mux 지연 : av_interleaved_write_frame 호출 지연
2) 타임 베이스 재스케일 정확성 (역행/음수 DTS 방지 카운트)
3) 송출 비트레이트, 송출 프레임률
4) SRT 송출 재전송 / 패킷 드롭

*/

void Controller::run_output_thread() {

    output_thread = std::thread([this]() {
        BOOST_LOG(debug) << "output thread start";
        AVPacket* pkt = nullptr;
        while (flag && encoder_queue.pop(pkt)) {

            int abr_idx = static_cast<int>(reinterpret_cast<intptr_t>(pkt->opaque));

            pkt->stream_index = output_streams[abr_idx]->index;
            pkt->pts = av_rescale_q(pkt->pts, enc_ctxs[abr_idx]->time_base, output_streams[abr_idx]->time_base);
            pkt->dts = av_rescale_q(pkt->dts, enc_ctxs[abr_idx]->time_base, output_streams[abr_idx]->time_base);
            pkt->duration = av_rescale_q(pkt->duration, enc_ctxs[abr_idx]->time_base, output_streams[abr_idx]->time_base);

            BOOST_LOG(debug) << "PKT : abr (" << abr_idx << ") " << pkt->pts << " " << pkt->dts << " " << pkt->duration << " " << pkt->time_base.num << "/" << pkt->time_base.den << " ";
            av_interleaved_write_frame(out_fmt_ctxs[abr_idx], pkt);
            // av_write_frame(out_fmt_ctx, pkt);
            av_packet_free(&pkt);
        }
        BOOST_LOG(debug) << "output loop exit";
        for(int i = 0; i < (int)out_fmt_ctxs.size(); i++)
            av_write_trailer(out_fmt_ctxs[i]);
    });
}

void Controller::create_streaming_pipeline() {
    flag = true;

    init_hw_device();
    init_filter();

    init_encoder();
    init_vmaf();

    init_input();
    init_decoder();
    init_output();

    run_input_thread();
    run_decoder_thread();
    run_filter_thread();
    run_encoder_thread();
    run_output_thread();

    input_thread.join();
    BOOST_LOG(debug) << "input_thread join finish";
    decoder_thread.join();
    BOOST_LOG(debug) << "decoder_thread join finish";
    filter_thread.join();
    BOOST_LOG(debug) << "filter_thread join finish";
    encoder_thread.join();
    BOOST_LOG(debug) << "encoder_thread join finish";
    output_thread.join();
    BOOST_LOG(debug) << "output_thread join finish";
}
