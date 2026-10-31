#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#if defined(__aarch64__)
#include <arm_neon.h>
#elif defined(__SSE2__)
#include <emmintrin.h>
#endif

namespace ft::self_model {


struct ModelCollisionLaneMask {
    template<bool Inclusive>
    static uint8_t compare(const std::array<float,4>& a,const std::array<float,4>& b) {
#if defined(__aarch64__)
        const auto av=vld1q_f32(a.data()),bv=vld1q_f32(b.data());
        const auto selected=Inclusive?vcleq_f32(av,bv):vcltq_f32(av,bv);
        const uint32_t bits[4]={1,2,4,8};
        return uint8_t(vaddvq_u32(vandq_u32(selected,vld1q_u32(bits))));
#elif defined(__SSE2__)
        const auto av=_mm_loadu_ps(a.data()),bv=_mm_loadu_ps(b.data());
        return uint8_t(_mm_movemask_ps(Inclusive?_mm_cmple_ps(av,bv):_mm_cmplt_ps(av,bv)));
#else
        uint8_t result=0;
        for(unsigned k=0;k<4;++k) if(Inclusive?a[k]<=b[k]:a[k]<b[k]) result|=1u<<k;
        return result;
#endif
    }
};


struct ModelCollisionPacket {
    std::array<float,4> x{},y{},z{};
    std::array<float,4> distance2(const float* low,const float* high) const {
        std::array<float,4> result;
#if defined(__aarch64__)
        const auto axis=[](const std::array<float,4>& p,float lo,float hi) {
            const auto v=vld1q_f32(p.data());
            return vmaxq_f32(vmaxq_f32(vsubq_f32(vdupq_n_f32(lo),v),
                vsubq_f32(v,vdupq_n_f32(hi))),vdupq_n_f32(0));
        };
        const auto dx=axis(x,low[0],high[0]),dy=axis(y,low[1],high[1]),dz=axis(z,low[2],high[2]);
        vst1q_f32(result.data(),vaddq_f32(vaddq_f32(vmulq_f32(dx,dx),vmulq_f32(dy,dy)),vmulq_f32(dz,dz)));
#elif defined(__SSE2__)
        const auto axis=[](const std::array<float,4>& p,float lo,float hi) {
            const auto v=_mm_loadu_ps(p.data());
            return _mm_max_ps(_mm_max_ps(_mm_sub_ps(_mm_set1_ps(lo),v),
                _mm_sub_ps(v,_mm_set1_ps(hi))),_mm_setzero_ps());
        };
        const auto dx=axis(x,low[0],high[0]),dy=axis(y,low[1],high[1]),dz=axis(z,low[2],high[2]);
        _mm_storeu_ps(result.data(),_mm_add_ps(_mm_add_ps(_mm_mul_ps(dx,dx),_mm_mul_ps(dy,dy)),_mm_mul_ps(dz,dz)));
#else
        for(size_t i=0;i<4;++i) {
            const auto axis=[](float p,float lo,float hi) { return std::max(std::max(lo-p,p-hi),0.f); };
            const float dx=axis(x[i],low[0],high[0]),dy=axis(y[i],low[1],high[1]),dz=axis(z[i],low[2],high[2]);
            result[i]=(dx*dx+dy*dy)+dz*dz;
        }
#endif
        return result;
    }
};



struct ModelCollisionBoundsPacket {
    ModelCollisionPacket low,high;
    uint8_t overlaps(const float* nodeLow,const float* nodeHigh) const {
#if defined(__aarch64__)
        const auto separated=[](const std::array<float,4>& lo,const std::array<float,4>& hi,float nlo,float nhi) {
            return vorrq_u32(vcltq_f32(vdupq_n_f32(nhi),vld1q_f32(lo.data())),
                vcgtq_f32(vdupq_n_f32(nlo),vld1q_f32(hi.data())));
        };
        const auto out=vorrq_u32(vorrq_u32(separated(low.x,high.x,nodeLow[0],nodeHigh[0]),
            separated(low.y,high.y,nodeLow[1],nodeHigh[1])),separated(low.z,high.z,nodeLow[2],nodeHigh[2]));
        const uint32_t bits[4]={1,2,4,8};
        return uint8_t(vaddvq_u32(vandq_u32(vmvnq_u32(out),vld1q_u32(bits))));
#elif defined(__SSE2__)
        const auto separated=[](const std::array<float,4>& lo,const std::array<float,4>& hi,float nlo,float nhi) {
            return _mm_or_ps(_mm_cmplt_ps(_mm_set1_ps(nhi),_mm_loadu_ps(lo.data())),
                _mm_cmpgt_ps(_mm_set1_ps(nlo),_mm_loadu_ps(hi.data())));
        };
        const auto out=_mm_or_ps(_mm_or_ps(separated(low.x,high.x,nodeLow[0],nodeHigh[0]),
            separated(low.y,high.y,nodeLow[1],nodeHigh[1])),separated(low.z,high.z,nodeLow[2],nodeHigh[2]));
        return uint8_t((~_mm_movemask_ps(out))&15);
#else
        uint8_t result=0;
        for(unsigned k=0;k<4;++k)
            if(!(nodeHigh[0]<low.x[k] || nodeLow[0]>high.x[k] ||
                 nodeHigh[1]<low.y[k] || nodeLow[1]>high.y[k] ||
                 nodeHigh[2]<low.z[k] || nodeLow[2]>high.z[k])) result|=1u<<k;
        return result;
#endif
    }
};
}
