#ifndef _FRAMEBUFFER_H_
#define _FRAMEBUFFER_H_
#include "block.h"

class FrameBuffer {
    Block *image_;
    uint32_t width_;
    uint32_t height_;
    uint32_t nblocks_;
    BlockMeta *meta_;
    uint8_t *storage_;
    uint8_t *meta_storage_;
    uint8_t *mb_types_;
    uint16_t *cbp_;
    int frame_num_;
    FrameBuffer(const FrameBuffer &other) = delete;
    FrameBuffer& operator=(const FrameBuffer&other) = delete;
    void destroy() {
        if (width_ && height_) {
            free(storage_);
            free(meta_);
        }
        memset(this, 0, sizeof(*this));
    }
 public:
    FrameBuffer() {
        image_ = nullptr;
        storage_ = nullptr;
        width_ = 0;
        height_ = 0;
        nblocks_ = 0;
    }
    void bzero() {
        memset(meta_, 0, sizeof(BlockMeta) * nblocks_);
        memset(image_, 0, sizeof(Block) * nblocks_);
    }
    void set_frame_num(int frame_num) {
        frame_num_ = frame_num;
    }
    bool is_same_frame(int frame_num) const {
        return frame_num_ == frame_num && width_ != 0 && height_ != 0;
    }
    uint32_t width()const {
        return width_;
    }
    uint32_t height()const {
        return height_;
    }
    void init(uint32_t width, uint32_t height, uint32_t nblocks) {
        height_ = height;
        width_ = width;
        nblocks_ = width * height;
        storage_ = (uint8_t*)malloc(nblocks_ * sizeof(Block) + 31);
        meta_storage_ = (uint8_t*)malloc(nblocks_ * sizeof(BlockMeta) + 31);
        size_t offset = storage_ - (uint8_t *)nullptr;
        if (offset & 32) {
            image_ = (Block*)(storage_ + 32 - (offset &31));
        } else { // already aligned
            image_ = (Block*)storage_;
        }
        offset = meta_storage_ - (uint8_t *)nullptr;
        if (offset & 32) {
            meta_ = (BlockMeta*)(meta_storage_ + 32 - (offset &31));
        } else { // already aligned
            meta_ = (BlockMeta*)meta_storage_;
        }
        bzero();
    }
    ~FrameBuffer() {
        destroy();
    }
    size_t block_allocated() const {
        return nblocks_;
    }
    Block& at(uint32_t x, uint32_t y) {
        return image_[x + y * width_];
    }
    const Block& at(uint32_t x, uint32_t y) const{
        return image_[x + y * width_];
    }
};
#endif
