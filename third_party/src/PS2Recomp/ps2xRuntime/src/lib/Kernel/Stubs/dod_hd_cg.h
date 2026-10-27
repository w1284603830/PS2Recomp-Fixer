#pragma once
// Direct DoD movie replacement. The original MPEG is PARSED for the native
// Sofdec/audio refill protocol, never decoded after selecting this route.
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <array>
#include <utility>
extern "C" {
#include <libavformat/avformat.h>
}

namespace dodhdcg {
constexpr uint32_t magic = 0x31474348; // HCG1; same wire layout in GS/DoDHdCg.h
struct Frame {
    std::vector<uint8_t> wire; // 16 uint32 header, native field preview, full HD RGBA
};
struct Info {
    int id=0, w=0, h=0, flags=0, x=0, y=0, cw=0, ch=0, count=0;
    uint64_t bytes=0;
    std::string file;
};
class Decoder {
    AVFormatContext* format=nullptr;
    AVCodecContext* codec=nullptr;
    AVCodecContext* parserContext=nullptr; // NO avcodec_open2: not a video decoder
    AVCodecParserContext* parser=nullptr;
    AVPacket* packet=nullptr;
    AVFrame* frame=nullptr;
    SwsContext* color=nullptr;
    SwsContext* previewScale=nullptr;
    int stream=-1;
    bool eof=false, parserFlushed=false;
    std::shared_ptr<Frame> first;
    std::vector<int64_t> pts;
    uint32_t decoded=0, served=0;
public:
    Info info;
    ~Decoder() {
        std::fprintf(stderr,"[cg-hd] close movie=%d decoded=%u handed=%u original_video_decodes=0\n",info.id,decoded,served);
        sws_freeContext(color); sws_freeContext(previewScale);
        av_frame_free(&frame); av_packet_free(&packet);
        if(parser) av_parser_close(parser);
        avcodec_free_context(&parserContext); avcodec_free_context(&codec);
        avformat_close_input(&format);
    }
    bool open(int movie, uint32_t nativeFlags) {
        std::filesystem::path root;
        if (const char* p=std::getenv("DOD_CG_HD_DIR")) {
            if (!*p || std::strcmp(p,"0")==0) return false;
            root=std::filesystem::u8path(p);
        } else {
            root=std::filesystem::path(dodpcsx2::hdCgDefaultRoot());
        }
        std::ifstream manifest(root/"movies.tsv");
        std::string line;
        bool found=false;
        while(std::getline(manifest,line)) {
            if(line.empty() || line[0]=='#') continue;
            Info row; std::istringstream in(line);
            if(in>>row.id>>row.w>>row.h>>row.flags>>row.x>>row.y>>row.cw>>row.ch>>row.count>>row.bytes>>row.file) {
                if(row.id==movie) { info=row; found=true; break; }
            }
        }
        if(!found || info.w!=512 || (info.h!=288 && info.h!=336) ||
           (info.flags&0x40)!=(nativeFlags&0x40) || info.count<=0 ||
           info.x!=0 || info.cw!=1920 || info.y<0 || info.ch<=0 || info.y+info.ch>1080 ||
           std::filesystem::path(info.file).filename()!=info.file) return false;
        std::error_code ec;
        auto path=root/info.file;
        if(std::filesystem::file_size(path,ec)!=info.bytes || ec) return false;
        std::ifstream times(root/("movie"+std::to_string(movie)+".pts"),std::ios::binary);
        pts.resize(info.count);
        if(!times.read(reinterpret_cast<char*>(pts.data()),pts.size()*sizeof(int64_t))) return false;
        auto name=path.u8string();
        if(avformat_open_input(&format,reinterpret_cast<const char*>(name.c_str()),nullptr,nullptr)<0 ||
           avformat_find_stream_info(format,nullptr)<0) return false;
        stream=av_find_best_stream(format,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
        if(stream<0) return false;
        auto* par=format->streams[stream]->codecpar;
        if(par->codec_id!=AV_CODEC_ID_HEVC || par->width!=1920 || par->height!=1080 ||
           format->streams[stream]->nb_frames!=info.count) return false;
        const AVCodec* dec=avcodec_find_decoder(par->codec_id);
        codec=avcodec_alloc_context3(dec);
        if(!codec || avcodec_parameters_to_context(codec,par)<0) return false;
        codec->thread_count=4;
        if(avcodec_open2(codec,dec,nullptr)<0) return false;
        parser=av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
        parserContext=avcodec_alloc_context3(nullptr);
        packet=av_packet_alloc(); frame=av_frame_alloc();
        if(!parser || !parserContext || !packet || !frame) return false;
        parserContext->codec_id=AV_CODEC_ID_MPEG2VIDEO;
        // Establish compatibility and decode the first HD picture BEFORE choosing the route.
        first=read();
        return first!=nullptr;
    }
    std::shared_ptr<Frame> read() {
        if(decoded>=static_cast<uint32_t>(info.count)) return {};
        for(;;) {
            int r=avcodec_receive_frame(codec,frame);
            if(r==0) break;
            if(r!=AVERROR(EAGAIN) || eof) return {};
            do {
                av_packet_unref(packet);
                r=av_read_frame(format,packet);
            } while(r>=0 && packet->stream_index!=stream);
            if(r<0) { eof=true; r=avcodec_send_packet(codec,nullptr); }
            else r=avcodec_send_packet(codec,packet);
            if(r<0) return {};
        }
        if(frame->width!=1920 || frame->height!=1080) return {};
        auto out=std::make_shared<Frame>();
        const size_t previewBytes=512u*(info.h/2)*4u;
        out->wire.resize(64+previewBytes+1920u*1080u*4u);
        uint32_t header[16]={magic,static_cast<uint32_t>(info.id),decoded,512u,static_cast<uint32_t>(info.h),
            static_cast<uint32_t>(info.flags),static_cast<uint32_t>(info.x),static_cast<uint32_t>(info.y),
            static_cast<uint32_t>(info.cw),static_cast<uint32_t>(info.ch),1920,1080};
        std::memcpy(out->wire.data(),header,64);
        uint8_t* hd=out->wire.data()+64+previewBytes;
        uint8_t* dest[]={hd}; int stride[]={1920*4};
        color=sws_getCachedContext(color,1920,1080,static_cast<AVPixelFormat>(frame->format),1920,1080,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
        if(!color) return {};
        const int* coeff=sws_getCoefficients(frame->colorspace==AVCOL_SPC_BT709?SWS_CS_ITU709:SWS_CS_DEFAULT);
        sws_setColorspaceDetails(color,coeff,frame->color_range==AVCOL_RANGE_JPEG,coeff,1,0,1<<16,1<<16);
        if(sws_scale(color,frame->data,frame->linesize,0,1080,dest,stride)!=1080) return {};
        for(size_t i=3;i<1920u*1080u*4u;i+=4) hd[i]=0x80;
        previewScale=sws_getCachedContext(previewScale,info.cw,info.ch,AV_PIX_FMT_RGBA,512,info.h/2,AV_PIX_FMT_RGBA,SWS_AREA,nullptr,nullptr,nullptr);
        if(!previewScale) return {};
        const uint8_t* cropped[]={hd+(info.y*1920+info.x)*4};
        uint8_t* low[]={out->wire.data()+64}; int lowStride[]={512*4};
        if(sws_scale(previewScale,cropped,stride,0,info.ch,low,lowStride)!=info.h/2) return {};
        for(size_t i=3;i<previewBytes;i+=4) out->wire[64+i]=0x80;
        ++decoded;
        av_frame_unref(frame);
        return out;
    }
    template<class Queue> bool append(Queue& queue) {
        auto hd=first?std::exchange(first,{}):read();
        if(!hd) return false;
        typename Queue::value_type out;
        out.width=info.w; out.height=info.h; out.pts90k=pts[served++]; out.hd=hd;
        out.rgba.resize(info.w*info.h*4);
        const uint8_t* field=hd->wire.data()+64;
        // Both native fields refer to this same HD frame; retain the original uploader/cadence.
        for(int y=0;y<info.h;++y) {
            int sy=(info.flags&0x40)?y%(info.h/2):y/2;
            std::memcpy(out.rgba.data()+y*info.w*4,field+sy*info.w*4,info.w*4);
        }
        queue.push_back(std::move(out));
        return true;
    }
    template<class Queue> bool feed(const uint8_t* data,size_t size,Queue& queue) {
        std::vector<uint8_t> padded(data,data+size);
        padded.resize(size+AV_INPUT_BUFFER_PADDING_SIZE,0);
        const uint8_t* p=padded.data();
        while(size) {
            uint8_t* parsed=nullptr; int bytes=0;
            int n=av_parser_parse2(parser,parserContext,&parsed,&bytes,p,static_cast<int>(size),AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
            if(n<0 || (n==0 && bytes==0)) return false;
            if(bytes && !append(queue)) return false;
            p+=n; size-=n;
        }
        return true;
    }
    template<class Queue> bool flush(Queue& queue) {
        if(parserFlushed) return true;
        parserFlushed=true;
        uint8_t* parsed=nullptr; int bytes=0;
        int r=av_parser_parse2(parser,parserContext,&parsed,&bytes,nullptr,0,AV_NOPTS_VALUE,AV_NOPTS_VALUE,0);
        if(r<0 || (bytes && !append(queue))) return false;
        if(served!=static_cast<uint32_t>(info.count)) {
            std::fprintf(stderr,"[cg-hd] COUNT MISMATCH movie=%d native=%u hd=%d\n",info.id,served,info.count);
            return false;
        }
        return true;
    }
};
}
