// ---------------------------------------------------------------------------
#include <set>
#include <fstream>
#include <sstream>
#include <iterator>
#include "region_render.hpp"
#include "SliceModel.h"
#include "glwidget.h"

RegionRender::~RegionRender(void)
{
    if(surface)
    {
        glwidget->glDeleteBuffers(1,&surface);
        surface = 0;
        glwidget->glDeleteBuffers(1,&surface_index);
        surface_index = 0;
    }
}
bool RegionRender::load(const std::vector<tipl::vector<3,short> >& seeds, tipl::matrix<4,4>& trans,unsigned char smooth)
{
    if(seeds.empty())
    {
        object.reset();
        return false;
    }
    bool need_trans = (trans != tipl::identity_matrix());

    tipl::vector<3,short> max_value(seeds[0]), min_value(seeds[0]);
    tipl::bounding_box_mt(seeds,max_value,min_value);

    center = max_value;
    center += min_value;
    center *= 0.5f;
    max_value += tipl::vector<3,short>(5, 5, 5);
    min_value -= tipl::vector<3,short>(5, 5, 5);
    tipl::shape<3> geo(uint32_t(max_value[0] - min_value[0]),
                          uint32_t(max_value[1] - min_value[1]),
                          uint32_t(max_value[2] - min_value[2]));
    float cur_scale = 1.0f;
    while(geo.width() > 256 || geo.height() > 256 || geo.depth() > 256)
    {
        cur_scale *= 2.0f;
        geo = tipl::shape<3>(geo[0]/2,geo[1]/2,geo[2]/2);
    }


    tipl::image<3,unsigned char> buffer(geo);
    tipl::par_for(seeds.size(),[&](unsigned int index)
    {
        tipl::vector<3,short> point(seeds[index]);
        point -= min_value;
        point /= cur_scale;
        if(buffer.shape().is_valid(point))
            buffer[tipl::pixel_index<3>(point[0], point[1], point[2],
                                     buffer.shape()).index()] = 200;
    });


    while(smooth)
    {
        tipl::filter::mean(buffer);
        --smooth;
    }
    object.reset(new tipl::march_cube(buffer, uint8_t(20)));
    tipl::vector<3,float> shift(min_value);
    tipl::par_for(object->point_list.size(),[&](unsigned int index)
    {
        if (cur_scale != 1.0f)
            object->point_list[index] *= cur_scale;
        object->point_list[index] += shift;
        if(need_trans)
            object->point_list[index].to(trans);
    });
    if (object->point_list.empty())
        object.reset();
    return object.get();
}

bool RegionRender::load(const tipl::image<3>& image_,
                       float threshold)
{
    tipl::image<3> image_buffer(image_);

    float scale = 1.0f;
    while(image_buffer.width() > 256 || image_buffer.height() > 256 || image_buffer.depth() > 256)
    {
        scale *= 2.0f;
        tipl::downsampling(image_buffer);
    }
    if (threshold == 0.0f)
    {
        float sum = 0;
        unsigned int num = 0;
        auto index_to = (image_buffer.size() >> 1) + image_buffer.shape().plane_size();
        for (auto index = (image_buffer.size() >> 1); index < index_to;++index)
        {
            float g = image_buffer[index];
            if (g > 0)
            {
                sum += g;
                ++num;
            }
        }
        if (!num)
            return false;
        threshold = sum / num * 0.85f;
    }
    object.reset(new tipl::march_cube(image_buffer,threshold));
    if (scale != 1.0f)
        tipl::multiply_constant(object->point_list,scale);
    if(object->point_list.empty())
        object.reset();
    return object.get();
}
// ---------------------------------------------------------------------------

bool RegionRender::load(unsigned int* buffer, tipl::shape<3>geo,
                       unsigned int threshold)
{
    tipl::image<3,unsigned char>re_buffer(geo);
    for (unsigned int index = 0; index < re_buffer.size(); ++index)
        re_buffer[index] = buffer[index] > threshold ? 200 : 0;

    tipl::filter::mean(re_buffer);
    object.reset(new tipl::march_cube(re_buffer, 50));
    if (object->point_list.empty())
        object.reset();
    return object.get();
}

void RegionRender::move_object(const tipl::vector<3,float>& shift)
{
    if(!object.get())
        return;
    tipl::add_constant(object->point_list,shift);

}

void RegionRender::trasnform_point_list(const tipl::matrix<4,4>& T)
{
    if(!object.get())
        return;
    auto& point_list = object->point_list;
    tipl::par_for(point_list.size(),[&](unsigned int i){
        point_list[i].to(T);
    });
}
void handleAlpha(tipl::rgb color,float alpha,int blend1,int blend2);
void RegionRender::draw(GLWidget* glwidget_,unsigned char cur_view,float alpha,int blend1,int blend2)
{
    if(!object.get())
        return;
    glwidget = glwidget_;
    if(!surface)
    {
        surface_block_size = object->point_list.size()*3*sizeof(float);
        glwidget->glGenBuffers(1,&surface);
        glwidget->glBindBuffer(GL_ARRAY_BUFFER, surface);
        glwidget->glBufferData(GL_ARRAY_BUFFER, surface_block_size/*vertices*/ + surface_block_size /*normal*/,0,GL_STATIC_DRAW);
        glwidget->glBufferSubData(GL_ARRAY_BUFFER, 0, surface_block_size,&object->point_list[0][0]);
        glwidget->glBufferSubData(GL_ARRAY_BUFFER, surface_block_size,surface_block_size,&object->normal_list[0][0]);
        glwidget->glGenBuffers(1,&surface_index);
        glwidget->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, surface_index);
        glwidget->glBufferData(GL_ELEMENT_ARRAY_BUFFER, object->sorted_index.size()*sizeof(unsigned int),&object->sorted_index[0],GL_STATIC_DRAW);
        object->point_list.clear();
        object->tri_list.clear();
        object->normal_list.clear();
        object->sorted_index.clear();
    }

    if(surface)
    {
        handleAlpha(color,alpha,blend1,blend2);
        glwidget->glBindBuffer(GL_ARRAY_BUFFER, surface);
        glwidget->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, surface_index);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_NORMAL_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, 0);
        glNormalPointer(GL_FLOAT, 0, (void*)surface_block_size);
        glDrawElements(GL_TRIANGLES, int(object->indices_count),
                       GL_UNSIGNED_INT,(void*)(cur_view*object->indices_count*sizeof(float)));
        glDisableClientState(GL_VERTEX_ARRAY);
        glDisableClientState(GL_NORMAL_ARRAY);
        glwidget->glBindBuffer(GL_ARRAY_BUFFER, 0);
        glwidget->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
}
