#pragma once
#include "self_model/model_pose.h"
#include "self_model/model_collision_worker.h"
#include "self_model/model_collision_packet.h"
#include <atomic>
#include <cstring>
#include <map>
#include <limits>
#include <numeric>
#include <tuple>

namespace ft::self_model {



class ModelSurfaceCollision {
    struct Triangle { std::array<uint32_t,3> v; uint16_t mask=1; };


    struct BodyShape {
        std::array<glm::vec3,3> points;
        glm::vec3 ab{0},ac{0},low{0},high{0},fallback{0,1,0},outward{0};
        float planeMargin=0;
        bool degenerate=false;
    };
    struct Node { glm::vec3 low{0},high{0}; int left=-1,right=-1; size_t begin=0,end=0; uint16_t mask=0; };
    struct Contact { glm::vec3 point{0},bary{0}; float distance2=0; bool found=false; uint32_t triangle=~uint32_t(0); };
    struct TriangleQuery { std::array<glm::vec3,3> points; bool unchanged=false; };
    struct TriangleResult {
        std::array<glm::vec3,3> before,after;
        size_t contacts=0;
        uint8_t dirty=0;
    };
    std::vector<Triangle> body_,cloth_;
    std::vector<BodyShape> bodyShapes_;
    std::vector<Node> tree_;
    std::vector<uint32_t> order_,skinVertices_,clothVertices_,weld_;
    std::vector<uint32_t> nearestTriangle_;
    std::vector<TriangleQuery> triangleQueries_;
    std::vector<TriangleResult> triangleResults_;
    std::vector<glm::vec3> vertexCorrections_;
    ModelCollisionWorkers workers_;
    std::vector<bool> parts_;
    std::vector<bool> dirty_;
    std::vector<uint16_t> masks_;
    std::vector<Vec3> positions_,normals_,basePositions_;
    std::vector<uint16_t> inputBones_;
    std::vector<glm::mat4> lastInputs_;
    bool resultValid_=false,reusedResult_=false;
    uint64_t revision_=0;
    std::vector<glm::vec3> normalSum_,baseNormalSum_;
    float padding_=0,maxDistance_=0;
    size_t contacts_=0;
    size_t speculativeTriangles_=0,recomputedTriangles_=0;
    uint8_t iterations_=8;

    static glm::vec3 closest(glm::vec3 p,const BodyShape& shape,glm::vec3& bary) {
        const auto a=shape.points[0],b=shape.points[1],c=shape.points[2];
        const auto ab=shape.ab,ac=shape.ac,ap=p-a;
        const float d1=glm::dot(ab,ap),d2=glm::dot(ac,ap);
        if(d1<=0 && d2<=0) { bary={1,0,0}; return a; }
        const auto bp=p-b; const float d3=glm::dot(ab,bp),d4=glm::dot(ac,bp);
        if(d3>=0 && d4<=d3) { bary={0,1,0}; return b; }
        const float vc=d1*d4-d3*d2;
        if(vc<=0 && d1>=0 && d3<=0) { const float v=d1/(d1-d3); bary={1-v,v,0}; return a+v*ab; }
        const auto cp=p-c; const float d5=glm::dot(ab,cp),d6=glm::dot(ac,cp);
        if(d6>=0 && d5<=d6) { bary={0,0,1}; return c; }
        const float vb=d5*d2-d1*d6;
        if(vb<=0 && d2>=0 && d6<=0) { const float w=d2/(d2-d6); bary={1-w,0,w}; return a+w*ac; }
        const float va=d3*d6-d5*d4;
        if(va<=0 && d4-d3>=0 && d5-d6>=0) {
            const float w=(d4-d3)/((d4-d3)+(d5-d6)); bary={0,1-w,w}; return b+w*(c-b);
        }
        const float sum=va+vb+vc;
        if(std::abs(sum)<1e-20f) { bary={1,0,0}; return a; }
        bary={va/sum,vb/sum,vc/sum}; return a*bary.x+b*bary.y+c*bary.z;
    }
    glm::vec3 center(uint32_t t) const {
        const auto& v=body_[t].v; return (vec(positions_[v[0]])+vec(positions_[v[1]])+vec(positions_[v[2]]))/3.f;
    }
    template<class Job> void parallelBatches(size_t count,size_t batch,Job job) {
        if(workers_.size()<3) { job(0,count); return; }
        std::atomic<size_t> next{0};
        workers_.run([&] {
            for(;;) {
                const size_t begin=next.fetch_add(batch,std::memory_order_relaxed);
                if(begin>=count) break;
                job(begin,std::min(begin+batch,count));
            }
        });
    }
    void updateBodyShapes(size_t begin,size_t end) {
        for(size_t i=begin;i<end;++i) {
            const auto& vertices=body_[i].v; auto& shape=bodyShapes_[i];
            for(int k=0;k<3;++k) shape.points[k]=vec(positions_[vertices[k]]);
            const auto a=shape.points[0],b=shape.points[1],c=shape.points[2];
            const auto magnitude=glm::abs(a);
            shape.planeMargin=1e-5f*(1.f+magnitude.x+magnitude.y+magnitude.z);
            shape.ab=b-a; shape.ac=c-a;
            shape.low=glm::min(a,glm::min(b,c)); shape.high=glm::max(a,glm::max(b,c));
            const auto cross=glm::cross(shape.ab,shape.ac);
            const float area=glm::length(cross);
            shape.degenerate=area<1e-10f;
            const bool normal=std::isfinite(area) && area>1e-6f;
            shape.fallback=normal?cross/area:glm::vec3(0,1,0);
            shape.outward=normal?shape.fallback:glm::vec3(0);
            const auto outside=vec(normals_[vertices[0]])+vec(normals_[vertices[1]])+vec(normals_[vertices[2]]);
            if(glm::dot(shape.outward,outside)<0) shape.outward=-shape.outward;
        }
    }
    int build(size_t begin,size_t end) {
        const int index=int(tree_.size()); tree_.push_back({});
        tree_[index].begin=begin; tree_[index].end=end;
        for(size_t i=begin;i<end;++i) tree_[index].mask|=body_[order_[i]].mask;
        if(end-begin>8) {
            auto low=center(order_[begin]),high=low;
            for(size_t i=begin+1;i<end;++i) { const auto c=center(order_[i]); low=glm::min(low,c); high=glm::max(high,c); }
            const auto extent=high-low; const int axis=extent.x>extent.y?(extent.x>extent.z?0:2):(extent.y>extent.z?1:2);
            const size_t mid=(begin+end)/2;
            std::nth_element(order_.begin()+begin,order_.begin()+mid,order_.begin()+end,[&](uint32_t a,uint32_t b){return center(a)[axis]<center(b)[axis];});
            const int left=build(begin,mid),right=build(mid,end);
            tree_[index].left=left; tree_[index].right=right;
        }
        return index;
    }
    void refit(int index) {
        auto& n=tree_[index];
        if(n.left>=0) {
            refit(n.left); refit(n.right);
            n.low=glm::min(tree_[n.left].low,tree_[n.right].low); n.high=glm::max(tree_[n.left].high,tree_[n.right].high);
        } else {
            n.low=glm::vec3(std::numeric_limits<float>::max()); n.high=-n.low;
            for(size_t i=n.begin;i<n.end;++i) {
                const auto& shape=bodyShapes_[order_[i]];
                n.low=glm::min(n.low,shape.low); n.high=glm::max(n.high,shape.high);
            }
        }
    }
    float distance2(int index,glm::vec3 p) const {
        const auto& n=tree_[index]; const auto d=glm::max(glm::max(n.low-p,p-n.high),glm::vec3(0)); return glm::dot(d,d);
    }
    void nearest(int index,glm::vec3 p,uint16_t mask,Contact& out) const {



        const auto& n=tree_[index];
        if(n.left>=0) {
            const float left=distance2(n.left,p),right=distance2(n.right,p);
            const bool leftFirst=left<right;
            const int first=leftFirst?n.left:n.right,second=leftFirst?n.right:n.left;
            if((tree_[first].mask&mask) && (leftFirst?left:right)<=out.distance2) nearest(first,p,mask,out);
            if((tree_[second].mask&mask) && (leftFirst?right:left)<=out.distance2) nearest(second,p,mask,out);
            return;
        }
        for(size_t i=n.begin;i<n.end;++i) {
            const auto& triangle=body_[order_[i]];
            if(!(triangle.mask&mask)) continue;
            const auto& shape=bodyShapes_[order_[i]];
            if(shape.degenerate) continue;
            const auto boxDistance=glm::max(glm::max(shape.low-p,p-shape.high),glm::vec3(0));


            if(glm::dot(boxDistance,boxDistance)>out.distance2*1.000001f+1e-12f) continue;
            glm::vec3 bary; const auto q=closest(p,shape,bary),d=p-q; const float d2=glm::dot(d,d);
            if(d2>=out.distance2) continue;
            out={q,bary,d2,true,order_[i]};
        }
    }
    struct QueryPacket {
        ModelCollisionPacket points;
        std::array<uint16_t,4> masks;
        std::array<Contact,4> hits;
        uint16_t commonMask=~uint16_t(0),combinedMask=0;
        uint8_t filter(uint8_t lanes,uint16_t mask,const std::array<float,4>& distances) const {
            if(!(mask&combinedMask)) return 0;
            if(!(mask&commonMask)) for(unsigned k=0;k<4;++k)
                if(!(masks[k]&mask)) lanes&=~(1u<<k);
            const std::array<float,4> limits={hits[0].distance2,hits[1].distance2,hits[2].distance2,hits[3].distance2};
            return lanes&ModelCollisionLaneMask::compare<true>(distances,limits);
        }
    };
    void nearestPacket(int index,QueryPacket& packet,uint8_t lanes) const {
        if(!(lanes&(lanes-1))) {
            unsigned k=0; while(!(lanes&(1u<<k))) ++k;
            nearest(index,{packet.points.x[k],packet.points.y[k],packet.points.z[k]},packet.masks[k],packet.hits[k]);
            return;
        }
        const auto& node=tree_[index];
        if(node.left>=0) {
            const auto& left=tree_[node.left]; const auto& right=tree_[node.right];
            const auto ld=packet.points.distance2(glm::value_ptr(left.low),glm::value_ptr(left.high));
            const auto rd=packet.points.distance2(glm::value_ptr(right.low),glm::value_ptr(right.high));
            const auto leftFirst=ModelCollisionLaneMask::compare<false>(ld,rd);


            const auto visit=[&](int child,uint8_t selected,const std::array<float,4>& distances) {
                if(!selected) return;
                const auto active=packet.filter(selected,tree_[child].mask,distances);
                if(active) nearestPacket(child,packet,active);
            };
            visit(node.left,lanes&leftFirst,ld);
            visit(node.right,lanes&~leftFirst,rd);
            visit(node.right,lanes&leftFirst,rd);
            visit(node.left,lanes&~leftFirst,ld);
            return;
        }
        for(size_t i=node.begin;i<node.end;++i) {
            const auto t=order_[i]; const auto& shape=bodyShapes_[t];
            if(shape.degenerate) continue;
            const auto distances=packet.points.distance2(glm::value_ptr(shape.low),glm::value_ptr(shape.high));
            for(unsigned k=0;k<4;++k) {
                if(!(lanes&(1u<<k)) || !(body_[t].mask&packet.masks[k])) continue;
                auto& hit=packet.hits[k];
                if(distances[k]>hit.distance2*1.000001f+1e-12f) continue;
                const glm::vec3 p(packet.points.x[k],packet.points.y[k],packet.points.z[k]);
                glm::vec3 bary; const auto q=closest(p,shape,bary),d=p-q;
                const float d2=glm::dot(d,d);
                if(d2>=hit.distance2) continue;
                hit={q,bary,d2,true,t};
            }
        }
    }
    glm::vec3 contactCorrection(glm::vec3 p,const Contact& hit) const {
        if(!hit.found) return glm::vec3(0);
        const auto& v=body_[hit.triangle].v;
        const auto normal=direction(vec(normals_[v[0]])*hit.bary.x+vec(normals_[v[1]])*hit.bary.y+
            vec(normals_[v[2]])*hit.bary.z,bodyShapes_[hit.triangle].fallback);
        const float signedDistance=glm::dot(p-hit.point,normal);
        if(signedDistance>=padding_) return glm::vec3(0);

        if(signedDistance>=0 && hit.distance2>padding_*padding_) return glm::vec3(0);
        return normal*std::min(padding_-signedDistance,maxDistance_*.25f);
    }
    float queryBound(glm::vec3 p,uint16_t mask,uint32_t previous) const {
        float result=maxDistance_*maxDistance_;


        if(previous<body_.size() && (body_[previous].mask&mask) && !bodyShapes_[previous].degenerate) {
            glm::vec3 bary;
            const auto delta=p-closest(p,bodyShapes_[previous],bary);
            const float bound=glm::dot(delta,delta)*1.000001f+1e-12f;
            if(std::isfinite(bound)) result=std::min(result,bound);
        }
        return result;
    }
    void correctPacket(size_t begin) {
        QueryPacket packet;
        for(unsigned k=0;k<4;++k) {
            const auto v=clothVertices_[begin+k]; const auto p=vec(positions_[v]);
            packet.points.x[k]=p.x; packet.points.y[k]=p.y; packet.points.z[k]=p.z;
            packet.masks[k]=masks_[v];
            packet.commonMask&=masks_[v]; packet.combinedMask|=masks_[v];
            packet.hits[k].distance2=queryBound(p,masks_[v],nearestTriangle_[v]);
        }
        const auto distances=packet.points.distance2(glm::value_ptr(tree_[0].low),glm::value_ptr(tree_[0].high));
        const auto lanes=packet.filter(15,tree_[0].mask,distances);
        if(lanes) nearestPacket(0,packet,lanes);
        for(unsigned k=0;k<4;++k) {
            const auto v=clothVertices_[begin+k];
            nearestTriangle_[v]=packet.hits[k].triangle;
            vertexCorrections_[begin+k]=contactCorrection(vec(positions_[v]),packet.hits[k]);
        }
    }
    glm::vec3 correction(glm::vec3 p,uint16_t mask,uint32_t& previous) const {
        Contact hit; hit.distance2=queryBound(p,mask,previous);
        if((tree_[0].mask&mask) && distance2(0,p)<=hit.distance2) nearest(0,p,mask,hit);
        previous=hit.triangle;
        return contactCorrection(p,hit);
    }
    static bool crosses(glm::vec3 start,glm::vec3 end,glm::vec3 origin,glm::vec3 e1,glm::vec3 e2) {
        const auto d=end-start,h=glm::cross(d,e2);
        const float determinant=glm::dot(e1,h);
        if(std::abs(determinant)<1e-10f) return false;
        const float inverse=1.f/determinant; const auto s=start-origin,q=glm::cross(s,e1);
        const float u=inverse*glm::dot(s,h),v=inverse*glm::dot(d,q),t=inverse*glm::dot(e2,q);
        return u>=0 && v>=0 && u+v<=1 && t>=0 && t<=1;
    }
    struct ClothShape {
        std::array<glm::vec3,3> points;
        glm::vec3 ab,ac,low,high;
        float planeMargin;
        void refresh() {
            ab=points[1]-points[0]; ac=points[2]-points[0];
            low=glm::min(points[0],glm::min(points[1],points[2]));
            high=glm::max(points[0],glm::max(points[1],points[2]));
            const auto magnitude=glm::abs(points[0]);
            planeMargin=1e-5f*(magnitude.x+magnitude.y+magnitude.z);
        }
    };
    void projectTriangle(int index,const Triangle& cloth,glm::vec3 low,glm::vec3 high,ClothShape& current,TriangleResult& result) const {
        const auto& node=tree_[index];
        if(!(node.mask&cloth.mask)) return;
        for(int k=0;k<3;++k) if(node.high[k]<low[k] || node.low[k]>high[k]) return;
        if(node.left>=0) {
            projectTriangle(node.left,cloth,low,high,current,result);
            projectTriangle(node.right,cloth,low,high,current,result); return;
        }
        projectLeaf(node,cloth,current,result);
    }

    [[gnu::always_inline]] inline void projectLeaf(const Node& node,const Triangle& cloth,ClothShape& current,TriangleResult& result) const {
        for(size_t i=node.begin;i<node.end;++i) {
            const auto& body=body_[order_[i]];
            if(!(body.mask&cloth.mask)) continue;
            const auto& shape=bodyShapes_[order_[i]];
            const auto a=current.points;
            const auto& b=shape.points;


            bool separated=false;
            for(int k=0;k<3;++k)
                separated|=shape.high[k]+1e-6f<current.low[k] || shape.low[k]-1e-6f>current.high[k];
            if(separated) continue;
            // Overlapping boxes still commonly lie on one side of the body
            // triangle's plane. Reject only a strict separation, with a margin
            // for floating point error; all near/coplanar cases keep the exact
            // existing six edge tests and their original correction order.
            const float da=glm::dot(a[0]-b[0],shape.outward);
            const float db=glm::dot(a[1]-b[0],shape.outward);
            const float dc=glm::dot(a[2]-b[0],shape.outward);
            const float margin=current.planeMargin+shape.planeMargin;
            if((da>margin && db>margin && dc>margin) ||
               (da<-margin && db<-margin && dc<-margin)) continue;
            bool hit=false;
            for(int k=0;k<3 && !hit;++k)
                hit=crosses(a[k],a[(k+1)%3],b[0],shape.ab,shape.ac) ||
                    crosses(b[k],b[(k+1)%3],a[0],current.ab,current.ac);
            if(!hit) continue;
            const auto normal=shape.outward;



            for(int k=0;k<3;++k) {
                const float depth=padding_-glm::dot(a[k]-b[0],normal);
                if(depth>0) {
                    const auto corrected=a[k]+normal*std::min(depth,maxDistance_*.25f);


                    for(int j=0;j<3;++j) if(cloth.v[j]==cloth.v[k]) current.points[j]=corrected;
                    result.dirty|=1u<<k; ++result.contacts;
                }
            }



            current.refresh();
        }
    }
    TriangleResult triangleResult(const Triangle& cloth) const {
        TriangleResult result;
        for(int k=0;k<3;++k) result.before[k]=vec(positions_[cloth.v[k]]);
        ClothShape current; current.points=result.before; current.refresh();
        projectTriangle(0,cloth,current.low,current.high,current,result);
        result.after=current.points;
        return result;
    }
    struct TrianglePacket {
        ModelCollisionBoundsPacket bounds;
        std::array<ClothShape,4> current;
        std::array<TriangleResult,4> results;
        size_t begin=0;
        uint16_t commonMask=~uint16_t(0),combinedMask=0;
    };
    void projectPacket(int index,TrianglePacket& packet,uint8_t lanes) const {
        if(!(lanes&(lanes-1))) {
            unsigned k=0; while(!(lanes&(1u<<k))) ++k;
            const auto& lo=packet.bounds.low; const auto& hi=packet.bounds.high;
            projectTriangle(index,cloth_[packet.begin+k],{lo.x[k],lo.y[k],lo.z[k]},
                {hi.x[k],hi.y[k],hi.z[k]},packet.current[k],packet.results[k]);
            return;
        }
        const auto& node=tree_[index];
        if(!(node.mask&packet.combinedMask)) return;
        if(!(node.mask&packet.commonMask)) for(unsigned k=0;k<4;++k)
            if(!(node.mask&cloth_[packet.begin+k].mask)) lanes&=~(1u<<k);
        if(!lanes) return;
        lanes&=packet.bounds.overlaps(glm::value_ptr(node.low),glm::value_ptr(node.high));
        if(!lanes) return;
        if(node.left>=0) {



            projectPacket(node.left,packet,lanes);
            projectPacket(node.right,packet,lanes);
        } else for(unsigned k=0;k<4;++k) if(lanes&(1u<<k))
            projectLeaf(node,cloth_[packet.begin+k],packet.current[k],packet.results[k]);
    }
    void trianglePacket(size_t begin) {
        TrianglePacket packet; packet.begin=begin;
        for(unsigned k=0;k<4;++k) {
            auto& current=packet.current[k]; auto& result=packet.results[k];
            packet.commonMask&=cloth_[begin+k].mask; packet.combinedMask|=cloth_[begin+k].mask;
            for(unsigned j=0;j<3;++j) result.before[j]=vec(positions_[cloth_[begin+k].v[j]]);
            current.points=result.before; current.refresh();
            packet.bounds.low.x[k]=current.low.x; packet.bounds.low.y[k]=current.low.y; packet.bounds.low.z[k]=current.low.z;
            packet.bounds.high.x[k]=current.high.x; packet.bounds.high.y[k]=current.high.y; packet.bounds.high.z[k]=current.high.z;
        }
        projectPacket(0,packet,15);
        for(unsigned k=0;k<4;++k) {
            packet.results[k].after=packet.current[k].points;
            triangleResults_[begin+k]=packet.results[k];
        }
    }
    static bool samePoint(const glm::vec3& a,const glm::vec3& b) {

        return std::memcmp(glm::value_ptr(a),glm::value_ptr(b),3*sizeof(float))==0;
    }
public:
    void clear() { *this=ModelSurfaceCollision{}; }
    bool active() const { return !tree_.empty(); }
    bool affects(size_t part) const { return active() && part<parts_.size() && parts_[part]; }
    size_t contacts() const { return contacts_; }
    size_t bodyTriangles() const { return body_.size(); }
    bool parallelQueries() const { return workers_.size()!=0; }
    size_t queryWorkers() const { return workers_.size(); }
    size_t speculativeTriangles() const { return speculativeTriangles_; }
    size_t recomputedTriangles() const { return recomputedTriangles_; }
    bool reusedResult() const { return reusedResult_; }
    uint64_t revision() const { return revision_; }
    const std::vector<Vec3>& positions() const { return positions_; }
    const std::vector<Vec3>& normals() const { return normals_; }
    void prepare(const Model& m) {
        clear(); parts_.resize(m.parts.size(),false);
        const bool declared=m.collisionHook.specified;
        if(declared && !m.collisionHook.enabled) return;
        std::vector<bool> skirtBones(m.bones.size(),false);
        std::vector<bool> bodyBone(m.bones.size(),false);
        for(size_t i=0;i<m.bones.size();++i) {
            const auto& b=m.bones[i]; skirtBones[i]=(b.flags&128) || (b.parent>=0 && skirtBones[b.parent]);
            bodyBone[i]=b.human>=0?(b.human<=8 || (b.human>=11 && b.human<=20) || b.human>=24):
                (b.parent>=0 && bodyBone[b.parent]);
        }
        for(size_t i=0;i<m.parts.size();++i) {
            const auto& p=m.parts[i];
            if(declared) { parts_[i]=p.collisionRole==CollisionRole::Deform; continue; }
            for(size_t j=p.firstVertex;j<p.firstVertex+p.vertexCount;++j) for(int k=0;k<4;++k)
                if(m.vertices[j].weights[k]>.01f && skirtBones[m.vertices[j].bones[k]]) parts_[i]=true;
        }
        if(std::find(parts_.begin(),parts_.end(),true)==parts_.end()) return;
        positions_.resize(m.vertices.size()); normals_.resize(m.vertices.size()); weld_.resize(m.vertices.size());
        masks_.resize(m.vertices.size(),1);
        std::iota(weld_.begin(),weld_.end(),0);
        using Key=std::tuple<float,float,float,std::array<uint16_t,4>,std::array<float,4>,uint16_t>;
        std::map<Key,uint32_t> welds;
        for(size_t i=0;i<m.vertices.size();++i) { positions_[i]=m.vertices[i].position; normals_[i]=m.vertices[i].normal; }
        for(size_t i=0;i<m.parts.size();++i) {
            const auto& p=m.parts[i]; bool body=declared && p.collisionRole==CollisionRole::Collider;


            if(!declared) for(const auto& [key,value]:m.materials[p.material].shaderFloats)
                if(key=="_SurfaceKind" && (value==1 || value==5)) body=true;
            if(parts_[i]) {
                for(uint32_t j=p.firstVertex;j<p.firstVertex+p.vertexCount;++j) {
                    const auto& v=m.vertices[j];
                    masks_[j]=declared?p.collisionMask:1;
                    const auto [it,inserted]=welds.emplace(Key{v.position.x,v.position.y,v.position.z,v.bones,v.weights,masks_[j]},j);
                    weld_[j]=it->second; if(inserted) clothVertices_.push_back(j);
                }
            }
            for(size_t j=p.firstIndex;j<p.firstIndex+p.indexCount;j+=3) {
                Triangle t; t.mask=declared?p.collisionMask:1;
                for(int k=0;k<3;++k) t.v[k]=p.firstVertex+m.indices[j+k];
                if(parts_[i]) { for(auto& v:t.v) v=weld_[v]; cloth_.push_back(t); }
                else if(body) {


                    bool relevant=declared;
                    if(!declared) for(auto v:t.v) {
                        float weight=0;
                        for(int k=0;k<4;++k) if(bodyBone[m.vertices[v].bones[k]]) weight+=m.vertices[v].weights[k];
                        relevant|=weight>.25f;
                    }
                    if(relevant) body_.push_back(t);
                }
            }
        }

        if(body_.empty() || cloth_.empty() || clothVertices_.size()>8192 || body_.size()>60000 || (declared && cloth_.size()>16384)) {
            clear();
            if(declared) throw std::runtime_error("Invalid collision hook geometry");
            return;
        }
        std::vector<bool> used(m.vertices.size(),false);
        for(const auto& t:body_) for(auto v:t.v) used[v]=true;
        for(size_t i=0;i<m.parts.size();++i) if(parts_[i]) {
            const auto& part=m.parts[i];
            for(size_t v=part.firstVertex;v<part.firstVertex+part.vertexCount;++v) used[v]=true;
        }

        for(size_t i=0;i<used.size();++i) if(used[i]) skinVertices_.push_back(uint32_t(i));
        std::vector<bool> inputBones(m.bones.size());
        for(auto v:skinVertices_) for(size_t k=0;k<4;++k)
            if(m.vertices[v].weights[k]) inputBones.at(m.vertices[v].bones[k])=true;
        for(size_t i=0;i<inputBones.size();++i) if(inputBones[i]) inputBones_.push_back(uint16_t(i));
        lastInputs_.resize(inputBones_.size());
        float low=positions_[0].y,high=low;
        for(const auto& p:positions_) { low=std::min(low,p.y); high=std::max(high,p.y); }
        const float height=high-low;
        if(!std::isfinite(height) || height<1e-5f) {
            clear();
            if(declared) throw std::runtime_error("Invalid collision hook model height");
            return;
        }
        padding_=height*(declared?m.collisionHook.paddingRatio:.002f);
        maxDistance_=height*(declared?m.collisionHook.maxDisplacementRatio:.12f);
        iterations_=declared?m.collisionHook.iterations:8;



        auto queryLow=vec(positions_[clothVertices_[0]]),queryHigh=queryLow;
        for(auto v:clothVertices_) { queryLow=glm::min(queryLow,vec(positions_[v])); queryHigh=glm::max(queryHigh,vec(positions_[v])); }
        const auto queryExtent=glm::max(queryHigh-queryLow,glm::vec3(1e-6f));
        const auto morton=[&](uint32_t v) {
            const auto point=glm::clamp((vec(positions_[v])-queryLow)/queryExtent,glm::vec3(0),glm::vec3(1))*1023.f;
            uint32_t code=0;
            for(unsigned axis=0;axis<3;++axis) for(unsigned bit=0;bit<10;++bit)
                code|=((uint32_t(point[axis])>>bit)&1u)<<(3*bit+axis);
            return code;
        };
        std::vector<std::pair<uint32_t,uint32_t>> queryOrder; queryOrder.reserve(clothVertices_.size());
        for(auto v:clothVertices_) queryOrder.emplace_back(morton(v),v);
        std::sort(queryOrder.begin(),queryOrder.end());
        for(size_t i=0;i<queryOrder.size();++i) clothVertices_[i]=queryOrder[i].second;
        normalSum_.resize(m.vertices.size());
        baseNormalSum_.resize(m.vertices.size()); basePositions_.resize(m.vertices.size());
        dirty_.resize(m.vertices.size(),false);
        order_.resize(body_.size()); std::iota(order_.begin(),order_.end(),0); tree_.reserve(body_.size()*2);
        nearestTriangle_.assign(m.vertices.size(),~uint32_t(0));
        triangleQueries_.resize(cloth_.size());
        vertexCorrections_.resize(clothVertices_.size());


        if(clothVertices_.size()>=512) {
            const auto cores=std::thread::hardware_concurrency();
            const size_t count=clothVertices_.size()>=2048 && cores>=6?3:1;
            workers_=ModelCollisionWorkers(count);
        }
        if(workers_.size()>=3) triangleResults_.resize(cloth_.size());
        bodyShapes_.resize(body_.size());
        build(0,body_.size());


        std::vector<Triangle> orderedBody; orderedBody.reserve(body_.size());
        for(auto t:order_) orderedBody.push_back(body_[t]);
        body_=std::move(orderedBody); std::iota(order_.begin(),order_.end(),0);
        updateBodyShapes(0,body_.size()); refit(0);
    }


    void update(const Model& m,const std::vector<glm::mat4>& matrices) {
        reusedResult_=false; speculativeTriangles_=recomputedTriangles_=0; if(!active()) return;



        bool same=resultValid_;
        resultValid_=false;
        for(size_t i=0;i<inputBones_.size() && same;++i)
            same=!std::memcmp(glm::value_ptr(lastInputs_[i]),glm::value_ptr(matrices.at(inputBones_[i])),16*sizeof(float));
        if(same) { resultValid_=true; reusedResult_=true; return; }
        contacts_=0;
        parallelBatches(skinVertices_.size(),128,[&](size_t begin,size_t end) {
            for(size_t i=begin;i<end;++i) {
                const auto v=skinVertices_[i];
                const auto skinned=skin(m.vertices[v],matrices);
                positions_[v]=basePositions_[v]=skinned.first; normals_[v]=skinned.second;
                if(!finite(vec(positions_[v])) || !finite(vec(normals_[v]))) throw std::runtime_error("Invalid surface collision pose");
            }
        });


        parallelBatches(body_.size(),128,[&](size_t begin,size_t end) { updateBodyShapes(begin,end); });
        refit(0);



        for(auto& query:triangleQueries_) query.unchanged=false;
        for(int pass=0;pass<iterations_;++pass) {
            const size_t before=contacts_;
            const auto queryVertices=[&](size_t begin,size_t end) {
                if(!pass) for(;begin+4<=end;begin+=4) correctPacket(begin);
                for(size_t i=begin;i<end;++i) {
                    const auto v=clothVertices_[i];
                    if(pass && !dirty_[v]) continue;
                    vertexCorrections_[i]=correction(vec(positions_[v]),masks_[v],nearestTriangle_[v]);
                }
            };



            if(parallelQueries() && !pass) {


                std::atomic<size_t> next{0};
                workers_.run([&] {
                    for(;;) {
                        const size_t begin=next.fetch_add(32,std::memory_order_relaxed);
                        if(begin>=clothVertices_.size()) break;
                        queryVertices(begin,std::min(begin+32,clothVertices_.size()));
                    }
                });
            } else queryVertices(0,clothVertices_.size());
            for(size_t i=0;i<clothVertices_.size();++i) {
                const auto v=clothVertices_[i];
                if(pass && !dirty_[v]) continue;
                const auto delta=vertexCorrections_[i];
                dirty_[v]=glm::dot(delta,delta)>padding_*padding_*1e-6f;
                if(dirty_[v]) { positions_[v]=vec(vec(positions_[v])+delta); ++contacts_; }
            }




            const bool speculative=!pass && workers_.size()>=3;
            if(speculative) speculativeTriangles_=cloth_.size();
            if(speculative) parallelBatches(cloth_.size(),32,[&](size_t begin,size_t end) {
                for(;begin+4<=end;begin+=4) trianglePacket(begin);
                for(size_t face=begin;face<end;++face) triangleResults_[face]=triangleResult(cloth_[face]);
            });
            for(size_t face=0;face<cloth_.size();++face) {
                const auto& t=cloth_[face]; auto& query=triangleQueries_[face];
                const auto a=vec(positions_[t.v[0]]),b=vec(positions_[t.v[1]]),c=vec(positions_[t.v[2]]);
                if(query.unchanged && query.points[0]==a && query.points[1]==b && query.points[2]==c) continue;
                TriangleResult result;
                if(speculative && samePoint(triangleResults_[face].before[0],a) &&
                   samePoint(triangleResults_[face].before[1],b) && samePoint(triangleResults_[face].before[2],c))
                    result=triangleResults_[face];
                else {
                    if(speculative) ++recomputedTriangles_;
                    result=triangleResult(t);
                }
                for(int k=0;k<3;++k) if(result.dirty&(1u<<k)) {
                    positions_[t.v[k]]=vec(result.after[k]); dirty_[t.v[k]]=true;
                }
                contacts_+=result.contacts;
                query.unchanged=result.contacts==0;
                if(query.unchanged) query.points={a,b,c};
            }


            for(auto v:clothVertices_) {
                const auto delta=vec(positions_[v])-vec(basePositions_[v]);
                if(glm::length(delta)>maxDistance_) positions_[v]=vec(vec(basePositions_[v])+direction(delta)*maxDistance_);
            }
            if(before==contacts_) break;
        }
        if(!contacts_) { rememberInputs(matrices); return; }


        for(auto v:clothVertices_) normalSum_[v]=baseNormalSum_[v]=glm::vec3(0);
        for(const auto& t:cloth_) {
            const auto n=glm::cross(vec(positions_[t.v[1]])-vec(positions_[t.v[0]]),vec(positions_[t.v[2]])-vec(positions_[t.v[0]]));
            const auto base=glm::cross(vec(basePositions_[t.v[1]])-vec(basePositions_[t.v[0]]),vec(basePositions_[t.v[2]])-vec(basePositions_[t.v[0]]));
            for(auto v:t.v) { normalSum_[v]+=n; baseNormalSum_[v]+=base; }
        }
        for(size_t i=0;i<m.parts.size();++i) if(parts_[i]) {
            const auto& p=m.parts[i];
            for(size_t v=p.firstVertex;v<p.firstVertex+p.vertexCount;++v) {
                const auto anchor=weld_[v]; positions_[v]=positions_[anchor];
                if(glm::length(normalSum_[anchor])>1e-8f && glm::length(baseNormalSum_[anchor])>1e-8f)
                    normals_[v]=vec(between(baseNormalSum_[anchor],normalSum_[anchor])*vec(normals_[v]));
                if(!finite(vec(positions_[v])) || !finite(vec(normals_[v]))) throw std::runtime_error("Invalid skirt contact result");
            }
        }
        rememberInputs(matrices);
    }
private:
    void rememberInputs(const std::vector<glm::mat4>& matrices) {
        for(size_t i=0;i<inputBones_.size();++i) lastInputs_[i]=matrices.at(inputBones_[i]);
        resultValid_=true; ++revision_;
    }
};
}
