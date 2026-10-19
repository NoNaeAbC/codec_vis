#include "text_atlas.hpp"
#include "paint_scene.hpp"
#include <cmath>
#include <map>
#include <tuple>
#include <stdexcept>
namespace codec_gui::gui {
std::vector<TextLine> wrap_text(TextShaper& shaper, std::string_view text, float width) {
    std::vector<TextLine> lines;
    if (!std::isfinite(width) || width <= 0) return lines;
    std::size_t paragraphStart = 0;
    do {
        auto paragraphEnd = text.find('\n', paragraphStart);
        if (paragraphEnd == std::string_view::npos) paragraphEnd = text.size();
        const auto paragraph = text.substr(paragraphStart, paragraphEnd - paragraphStart);
        if (paragraph.empty()) lines.push_back({"", 0});
        const auto shaped = shaper.shape(paragraph);
        std::map<std::size_t, float> advances;
        for (const auto& glyph : shaped.glyphs) advances[glyph.cluster] += std::abs(glyph.advanceX);
        advances[paragraph.size()] = 0;
        std::size_t start = 0;
        while (start < paragraph.size()) {
            float used = 0;
            std::size_t end = start, space = start;
            for (auto it = advances.lower_bound(start); it != advances.end() && it->first < paragraph.size(); ++it) {
                if (used + it->second > width && end > start) break;
                used += it->second;
                end = std::next(it)->first;
                if (paragraph[it->first] == ' ' || paragraph[it->first] == '\t') space = end;
                if (used > width) break; // A single oversize cluster is clipped, never split.
            }
            if (end <= start) end = paragraph.size();
            if (end < paragraph.size() && space > start) end = space;
            auto segment = paragraph.substr(start, end - start);
            while (!segment.empty() && (segment.back() == ' ' || segment.back() == '\t')) segment.remove_suffix(1);
            lines.push_back({std::string(segment), shaper.shape(segment).width});
            start = end;
            while (start < paragraph.size() && (paragraph[start] == ' ' || paragraph[start] == '\t')) ++start;
        }
        if (paragraphEnd == text.size()) break;
        paragraphStart = paragraphEnd + 1;
    } while (paragraphStart <= text.size());
    return lines;
}
namespace {
struct AtlasFull {};
struct PackedGlyph { int x=0,y=0,w=0,h=0,bearingX=0,bearingY=0; };
using GlyphKey = std::tuple<uint64_t, uint32_t, uint32_t>;
}
struct TextAtlasCache::Impl {
    TextAtlas atlas;
    std::map<GlyphKey, PackedGlyph> glyphs;
    std::map<std::pair<FontRole,int>,std::unique_ptr<TextShaper>> fonts;
    int x=1,y=1,rowHeight=0;
    uint64_t source=0, rasterized=0;
    float scale=1;
    TextShaper& font(const DrawCommand& command,TextShaper& fallback) {
        if (command.fontSize <= 0 && command.fontRole == FontRole::Interface) return fallback;
        const int size = int(std::lround((command.fontSize > 0 ? command.fontSize : 16)*scale*64));
        auto key=std::pair{command.fontRole,size};
        auto& face=fonts[key];
        if (!face) face=std::make_unique<TextShaper>(command.fontRole==FontRole::Monospace ? default_monospace_font_path() : default_font_path(),float(size)/64);
        return *face;
    }
    const PackedGlyph& ensure(TextShaper& font,const ShapedGlyph& shape) {
        GlyphKey key{font.identity(),shape.fontIndex,shape.glyphIndex};
        if (auto it=glyphs.find(key);it!=glyphs.end()) return it->second;
        auto bitmap=font.rasterize_glyph(shape);++rasterized;
        if (bitmap.width+4>atlas.width || bitmap.height+4>4096) throw AtlasFull{};
        if(x+bitmap.width+2>atlas.width){x=1;y+=rowHeight+2;rowHeight=0;}
        if(y+bitmap.height+2>4096)throw AtlasFull{};
        const int needed=y+bitmap.height+2;
        while(atlas.height<needed)atlas.height=std::min(4096,atlas.height*2);
        atlas.alpha.resize(std::size_t(atlas.width)*atlas.height,0);
        PackedGlyph packed{x+1,y+1,bitmap.width,bitmap.height,bitmap.bearingX,bitmap.bearingY};
        for(int row=0;row<bitmap.height;++row)
            std::copy_n(bitmap.alpha.data()+std::size_t(row)*bitmap.width,bitmap.width,
                        atlas.alpha.data()+std::size_t(packed.y+row)*atlas.width+packed.x);
        x+=bitmap.width+2;rowHeight=std::max(rowHeight,bitmap.height);
        return glyphs.emplace(key,packed).first->second;
    }
};
TextAtlasCache::TextAtlasCache():impl_(std::make_unique<Impl>()){}
TextAtlasCache::~TextAtlasCache()=default;
void TextAtlasCache::clear(){*impl_=Impl{};}
TextAtlas& TextAtlasCache::atlas(){return impl_->atlas;}
uint64_t TextAtlasCache::rasterized_glyph_count()const{return impl_->rasterized;}
const TextAtlas& TextAtlasCache::build(const std::vector<DrawCommand>& commands,TextShaper& fallback,int width,float scale) {
    width=std::clamp(width,64,4096);
    if(!std::isfinite(scale) || scale<=0)throw std::invalid_argument("Invalid UI device scale");
    if(impl_->source!=fallback.identity() || impl_->scale!=scale || impl_->atlas.width!=width)clear();
    for(int attempt=0;attempt<2;++attempt) try {
        auto& cache=*impl_;
        cache.source=fallback.identity();cache.scale=scale;
        auto& atlas=cache.atlas;
        atlas.width=width;
        if(!atlas.height){atlas.height=64;atlas.alpha.resize(std::size_t(width)*64);}
        atlas.quads.clear();
        struct Line { DrawCommand command; Rect clip; std::size_t index; TextShaper* font; ShapedText shaped,ellipsis; };
        std::vector<Line> lines;
        for(const auto& item:compile_paint_scene(commands)) {
            const auto& command=commands[item.commandIndex];
            if(command.kind!=DrawCommandKind::Text || command.text.empty() || !ui::valid_rect(command.rect) ||
               !ui::valid_rect(ui::intersect(command.rect,item.clip)))continue;
            auto& font=cache.font(command,fallback);
            std::vector<TextLine> segments;
            if(command.textOverflow==TextOverflow::Wrap)segments=wrap_text(font,command.text,command.rect.w*scale);
            else segments.push_back({command.text,0});
            float y=command.rect.y;
            const float lineHeight=command.lineHeight>0?command.lineHeight:font.shape("Mg").height/scale*1.35f;
            for(const auto& segment:segments) {
                if(y>=command.rect.y+command.rect.h)break;
                auto lineCommand=command;
                lineCommand.text=segment.text;
                lineCommand.rect.y=y;
                lineCommand.rect.h=std::min(lineHeight,command.rect.y+command.rect.h-y);
                auto shaped=font.shape(segment.text);
                auto ellipsis=font.shape("...");
                for(const auto& glyph:shaped.glyphs)cache.ensure(font,glyph);
                if(command.textOverflow==TextOverflow::Ellipsis)for(const auto& glyph:ellipsis.glyphs)cache.ensure(font,glyph);
                lines.push_back({std::move(lineCommand),item.clip,item.commandIndex,&font,std::move(shaped),std::move(ellipsis)});
                y+=lineHeight;
            }
        }
        // Packing is complete before UV normalization: growth never invalidates this frame.
        for(const auto& line:lines) {
            const auto& command=line.command;
            const float physicalWidth=command.rect.w*scale;
            const bool elide=command.textOverflow==TextOverflow::Ellipsis && line.shaped.width>physicalWidth;
            const float limit=elide?std::max(0.f,physicalWidth-line.ellipsis.width):physicalWidth;
            const float displayedWidth=elide?physicalWidth:std::min(physicalWidth,line.shaped.width);
            float offset=command.textAlign==TextAlign::Right?physicalWidth-displayedWidth:
                         command.textAlign==TextAlign::Center?(physicalWidth-displayedWidth)/2:0;
            auto append=[&](const ShapedGlyph& glyph,float pen,bool elision) {
                const auto& p=cache.glyphs.at({line.font->identity(),glyph.fontIndex,glyph.glyphIndex});
                if(p.w<=0 || p.h<=0)return;
                Rect r{std::round(command.rect.x*scale+offset+pen+glyph.offsetX+p.bearingX)/scale,
                       std::round(command.rect.y*scale+line.shaped.height-glyph.offsetY-p.bearingY)/scale,float(p.w)/scale,float(p.h)/scale};
                const auto visible=ui::intersect(r,ui::intersect(command.rect,line.clip));
                if(!ui::valid_rect(visible))return;
                Rect uv{(p.x+(visible.x-r.x)*scale)/atlas.width,(p.y+(visible.y-r.y)*scale)/atlas.height,
                        visible.w*scale/atlas.width,visible.h*scale/atlas.height};
                atlas.quads.push_back({visible,uv,command.color,elision,command.layer,line.clip,line.index});
            };
            float pen=0;
            for(std::size_t i=0;i<line.shaped.glyphs.size();) {
                std::size_t end=i+1;
                float advance=line.shaped.glyphs[i].advanceX;
                while(end<line.shaped.glyphs.size() && line.shaped.glyphs[end].cluster==line.shaped.glyphs[i].cluster)
                    advance+=line.shaped.glyphs[end++].advanceX;
                if(elide && pen+advance>limit)break;
                for(;i<end;++i){append(line.shaped.glyphs[i],pen,false);pen+=line.shaped.glyphs[i].advanceX;}
            }
            if(elide)for(const auto& glyph:line.ellipsis.glyphs){append(glyph,pen,true);pen+=glyph.advanceX;}
        }
        return atlas;
    } catch(const AtlasFull&) {
        clear();
        if(attempt)throw std::runtime_error("Visible UI glyphs exceed the 4096-pixel atlas capacity");
    }
    throw std::logic_error("Unreachable atlas build");
}
TextAtlas build_text_atlas(const std::vector<DrawCommand>& commands,TextShaper& shaper,int width,float scale) {
    TextAtlasCache cache;return cache.build(commands,shaper,width,scale);
}
} // namespace codec_gui::gui
