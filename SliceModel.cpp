// ---------------------------------------------------------------------------
#include <string>
#include <filesystem>
#include <QFileInfo>
#include <QImage>
#include <QInputDialog>
#include <QMessageBox>
#include <QDir>
#include "SliceModel.h"
#include "fib_data.hpp"
#include "reg.hpp"
SliceModel::SliceModel(fib_data* handle_,uint32_t view_id_):handle(handle_),view_id(view_id_)
{
    slice_visible[0] = false;
    slice_visible[1] = false;
    slice_visible[2] = false;
    to_dif.identity();
    to_slice.identity();
    dim = handle_->dim;
    vs = handle_->vs;
    trans_to_mni = handle->trans_to_mni;
    slice_pos[0] = dim.width() >> 1;
    slice_pos[1] = dim.height() >> 1;
    slice_pos[2] = dim.depth() >> 1;
}
// ---------------------------------------------------------------------------
void SliceModel::apply_overlay(tipl::color_image& show_image,
                    unsigned char cur_dim,
                    std::shared_ptr<SliceModel> other_slice) const
{
    if(show_image.empty())
        return;
    bool op = show_image[0][0] < 128;
    const auto& v2c = other_slice->handle->view_item[other_slice->view_id].v2c;
    std::pair<float,float> range = other_slice->get_contrast_range();
    for(int y = 0,pos = 0;y < show_image.height();++y)
        for(int x = 0;x < show_image.width();++x,++pos)
        {
            auto v = toOtherSlice(other_slice,cur_dim,x,y);
            float value = 0;
            if(!tipl::estimate(other_slice->get_source(),v,value))
                continue;
            if((value > 0.0f && value > range.first) ||
               (value < 0.0f && value < range.second))
            {
                if(op)
                    show_image[pos] |= v2c[value];
                else
                    show_image[pos] &= v2c[value];
            }

        }
}


// ---------------------------------------------------------------------------
std::pair<float,float> SliceModel::get_value_range(void) const
{
    if(handle->view_item[view_id].max_value == 0.0f)
        handle->view_item[view_id].get_minmax();
    return std::make_pair(handle->view_item[view_id].min_value,handle->view_item[view_id].max_value);
}
// ---------------------------------------------------------------------------
std::pair<float,float> SliceModel::get_contrast_range(void) const
{
    return std::make_pair(handle->view_item[view_id].contrast_min,handle->view_item[view_id].contrast_max);
}
// ---------------------------------------------------------------------------
std::pair<unsigned int,unsigned int> SliceModel::get_contrast_color(void) const
{
    return std::make_pair(handle->view_item[view_id].min_color,handle->view_item[view_id].max_color);
}
// ---------------------------------------------------------------------------
void SliceModel::set_contrast_range(float min_v,float max_v)
{
    handle->view_item[view_id].contrast_min = min_v;
    handle->view_item[view_id].contrast_max = max_v;
    handle->view_item[view_id].v2c.set_range(min_v,max_v);
}
// ---------------------------------------------------------------------------
void SliceModel::set_contrast_color(unsigned int min_c,unsigned int max_c)
{
    handle->view_item[view_id].min_color = min_c;
    handle->view_item[view_id].max_color = max_c;
    handle->view_item[view_id].v2c.two_color(min_c,max_c);
}
// ---------------------------------------------------------------------------
void SliceModel::get_slice(tipl::color_image& show_image,unsigned char cur_dim,int pos,
                           const std::vector<std::shared_ptr<SliceModel> >& overlay_slices) const
{
    handle->get_slice(view_id,cur_dim, pos,show_image);
    for(auto overlay_slice : overlay_slices)
        if(this != overlay_slice.get())
            apply_overlay(show_image,cur_dim,overlay_slice);
}
// ---------------------------------------------------------------------------
void SliceModel::get_high_reso_slice(tipl::color_image& show_image,unsigned char cur_dim,int pos) const
{
    if(handle && handle->has_high_reso)
    {
        handle->high_reso->view_item[view_id].v2c = handle->view_item[view_id].v2c;
        handle->high_reso->get_slice(view_id,cur_dim, pos*int(handle->high_reso->dim[cur_dim])/int(handle->dim[cur_dim]),show_image);
    }
}
// ---------------------------------------------------------------------------
tipl::const_pointer_image<3> SliceModel::get_source(void) const
{
    return handle->view_item[view_id].get_image();
}
// ---------------------------------------------------------------------------
std::string SliceModel::get_name(void) const
{
    return handle->view_item[view_id].name;
}
// ---------------------------------------------------------------------------
CustomSliceModel::CustomSliceModel(fib_data* new_handle):
    SliceModel(new_handle,0)
{
    is_diffusion_space = false;
}
// ---------------------------------------------------------------------------
void CustomSliceModel::get_slice(tipl::color_image& image,
                           unsigned char cur_dim,int pos,
                           const std::vector<std::shared_ptr<SliceModel> >& overlay_slices) const
{
    if(!picture.empty() && (dim[cur_dim] != picture.width() && dim[cur_dim] != picture.height()))
        image = picture;
    else
        return SliceModel::get_slice(image,cur_dim,pos,overlay_slices);
}
// ---------------------------------------------------------------------------
tipl::const_pointer_image<3> CustomSliceModel::get_source(void) const
{
    return tipl::const_pointer_image<3>(source_images.empty() ? (const float*)(0) : &source_images[0],source_images.shape());
}
// ---------------------------------------------------------------------------
void initial_LPS_nifti_srow(tipl::matrix<4,4>& T,const tipl::shape<3>& geo,const tipl::vector<3>& vs);
void prepare_idx(const char* file_name,std::shared_ptr<tipl::io::gz_istream> in);
void save_idx(const char* file_name,std::shared_ptr<tipl::io::gz_istream> in);
bool parse_age_sex(const std::string& file_name,std::string& age,std::string& sex);
QString get_matched_demo(QWidget *parent,std::shared_ptr<fib_data>);
bool CustomSliceModel::load_slices(const std::vector<std::string>& files,bool is_mni)
{
    if(files.empty())
        return false;
    // QSDR loaded, use MNI transformation instead
    bool has_transform = false;
    source_file_name = files[0].c_str();
    name = QFileInfo(files[0].c_str()).completeBaseName().remove(".nii").toStdString();
    to_dif.identity();
    to_slice.identity();
    tipl::progress prog("load slices ",std::filesystem::path(files[0]).filename().string().c_str());
    // picture as slice
    if(QFileInfo(files[0].c_str()).suffix() == "bmp" ||
       QFileInfo(files[0].c_str()).suffix() == "jpg" ||
       QFileInfo(files[0].c_str()).suffix() == "png" ||
       QFileInfo(files[0].c_str()).suffix() == "tif" ||
       QFileInfo(files[0].c_str()).suffix() == "tiff")

    {
        QString info_file = QString(files[0].c_str()) + ".info.txt";
        if(files.size() == 1) // single slice
        {
            uint32_t slices_count = 10;
            {
                QImage in;
                if(!in.load(files[0].c_str()))
                {
                    error_msg = "invalid image format: ";
                    error_msg += files[0];
                    return false;
                }
                QImage buf = in.convertToFormat(QImage::Format_RGB32);
                picture.resize(tipl::shape<2>(uint32_t(in.width()),uint32_t(in.height())));
                source_images.resize(tipl::shape<3>(uint32_t(in.width()),uint32_t(in.height()),slices_count));
                const uchar* ptr = buf.bits();
                for(size_t j = 0;j < source_images.plane_size();++j,ptr += 4)
                {
                    picture[j] = tipl::rgb(*(ptr+2),*(ptr+1),*ptr);
                    for(size_t k = 0,pos = j;k < slices_count;++k,pos += source_images.plane_size())
                        source_images[pos] = float(*ptr);
                }
            }

            vs = handle->vs*(handle->dim.width())/(source_images.width());

            tipl::transformation_matrix<float> M(arg_min,handle->dim,handle->vs,source_images.shape(),vs);
            M.save_to_transform(to_slice.begin());
            to_dif = tipl::inverse(to_slice);
            initial_LPS_nifti_srow(trans_to_mni,source_images.shape(),vs);
            has_transform = true;
        }
        else
        {
            QImage in;
            if(!in.load(files[0].c_str()))
            {
                error_msg = "invalid image format: ";
                error_msg += files[0];
                return false;
            }

            dim[0] = in.width();
            dim[1] = in.height();
            dim[2] = uint32_t(files.size());
            vs[0] = vs[1] = vs[2] = 1.0f;

            try{
                source_images.resize(dim);
            }
            catch(...)
            {
                error_msg = "Memory allocation failed. Please increase downsampling count";
                return false;
            }

            for(size_t file_index = 0;prog(file_index,dim[2]);++file_index)
            {
                QImage in;
                if(!in.load(files[file_index].c_str()))
                {
                    error_msg = "invalid image format: ";
                    error_msg += files[file_index];
                    return false;
                }
                QImage buf = in.convertToFormat(QImage::Format_RGB32).mirrored();
                tipl::image<2,short> I(tipl::shape<2>(in.width(),in.height()));
                const uchar* ptr = buf.bits();
                for(size_t j = 0;j < I.size();++j,ptr += 4)
                    I[j] = *ptr;

                std::copy(I.begin(),I.end(),source_images.begin() +
                          long(file_index*source_images.plane_size()));
            }
            if(prog.aborted())
                return false;
            has_transform = true;
        }
    }
    // load and match demographics DB file
    if(source_images.empty() && QString(files[0].c_str()).endsWith(".db.fib.gz"))
    {
        std::shared_ptr<fib_data> db_handle(new fib_data);
        if(!db_handle->load_from_file(files[0].c_str()) || !db_handle->db.has_db())
        {
            error_msg = db_handle->error_msg;
            return false;
        }

        {
            std::string age,sex,demo;
            if(parse_age_sex(QFileInfo(handle->fib_file_name.c_str()).baseName().toStdString(),age,sex))
                demo = age+" "+sex;
            if(!handle->demo.empty())
                demo = handle->demo;
            if(demo.empty() && tipl::show_prog)
                demo = get_matched_demo(nullptr,db_handle).toStdString();
            tipl::out() << "subject's demo: " << demo;
            if(!db_handle->db.get_demo_matched_volume(demo,source_images))
            {
                error_msg = db_handle->db.error_msg;
                return false;
            }
        }
        if(!handle->mni2sub(source_images,db_handle->trans_to_mni))
        {
            error_msg = handle->error_msg;
            return false;
        }
        is_diffusion_space = true;
        has_transform = true;
    }


    // load nifti file
    if(source_images.empty() &&
       (QString(files[0].c_str()).endsWith("nii.gz") || QString(files[0].c_str()).endsWith("nii")))
    {
        tipl::io::gz_nifti nifti;
        //  prepare idx file
        prepare_idx(files[0].c_str(),nifti.input_stream);
        if(!nifti.load_from_file(files[0]))
        {
            error_msg = nifti.error_msg;
            return false;
        }
        nifti.toLPS(source_images,prog);
        save_idx(files[0].c_str(),nifti.input_stream);
        nifti.get_voxel_size(vs);
        nifti.get_image_transformation(trans_to_mni);
        if(handle->is_mni)
        {
            tipl::out() << "Assuming the slices are already in the template space." << std::endl;
            to_slice = tipl::inverse(to_dif = tipl::from_space(trans_to_mni).to(handle->trans_to_mni));
            has_transform = true;
        }
        else
        {
            if(source_images.shape() != handle->dim)
            {
                if(QFileInfo(files[0].c_str()).fileName().toLower().contains("mni"))
                {
                    tipl::out() << std::filesystem::path(files[0]).filename().string() <<
                                 " has 'mni' in the file name and has a different image size from DWI. It will be spatially normalized from template space to native space." << std::endl;
                    is_mni = true;
                }
                if(is_mni)
                {
                    tipl::out() << "Warping template-space slices to the subject space." << std::endl;
                    if(!handle->mni2sub(source_images,trans_to_mni))
                    {
                        error_msg = handle->error_msg;
                        return false;
                    }
                    is_diffusion_space = true;
                    trans_to_mni = handle->trans_to_mni;
                    has_transform = true;
                }
            }
            else
            // slice and DWI have the same image size
            {
                if(QFileInfo(files[0].c_str()).fileName().contains("reg"))
                {
                    tipl::out() << "The slices have the same dimension, and there is 'reg' in the file name." << std::endl;
                    tipl::out() << "no registration needed." << std::endl;
                    is_diffusion_space = true;
                    trans_to_mni = handle->trans_to_mni;
                    has_transform = true;
                }
                else
                    tipl::out() << "registration will be applied even though the image size is identical. To disable registration, add 'reg' to the file name. " << std::endl;
            }
        }
    }

    // bruker images
    if(source_images.empty())
    {
        tipl::io::bruker_2dseq bruker;
        if(bruker.load_from_file(files[0].c_str()))
        {
            bruker.get_voxel_size(vs);
            source_images = std::move(bruker.get_image());
            initial_LPS_nifti_srow(trans_to_mni,source_images.shape(),vs);
            QDir d = QFileInfo(files[0].c_str()).dir();
            if(d.cdUp() && d.cdUp())
            {
                QString method_file_name = d.absolutePath()+ "/method";
                tipl::io::bruker_info method;
                if(method.load_from_file(method_file_name.toStdString().c_str()))
                    name = method["Method"];
            }
        }
    }

    // dicom images
    if(source_images.empty())
    {
        tipl::io::dicom_volume volume;
        if(!volume.load_from_files(files))
        {
            error_msg = volume.error_msg;
            return false;
        }
        volume.get_voxel_size(vs);
        volume.save_to_image(source_images);
        initial_LPS_nifti_srow(trans_to_mni,source_images.shape(),vs);
    }

    if(source_images.empty())
    {
        error_msg = "failed to load image volume.";
        return false;
    }
    // add image to the view item lists
    {
        update_image();
        tipl::out() << "add new slices: " << name << std::endl;
        tipl::out() << "dimension: " << source_images.shape() << std::endl;
        if(source_images.shape() == handle->dim)
            tipl::out() << "The slices have the same dimension as DWI." << std::endl;
        handle->view_item.push_back(item(name,&*source_images.begin(),source_images.shape()));
        view_id = uint32_t(handle->view_item.size()-1);
    }


    if(source_images.shape() == handle->dim && !has_transform)
    {
        if(handle->is_mni || QFileInfo(files[0].c_str()).fileName().toLower().contains("native"))
        {
            tipl::out() << "No registration required." << std::endl;
            is_diffusion_space = true;
            trans_to_mni = handle->trans_to_mni;
            has_transform = true;
        }
        else
            tipl::out() << "Registration will be applied. To disable registration, add 'native' to the file name." << std::endl;
    }

    if(!has_transform && handle->dim.depth() < 10) // 2d assume FOV is the same
    {
        to_slice[0] = float(source_images.width())/float(handle->dim.width());
        to_slice[5] = float(source_images.height())/float(handle->dim.height());
        to_slice[10] = float(source_images.depth())/float(handle->dim.depth());
        to_slice[15] = 1.0;
        to_dif = tipl::inverse(to_slice);
        has_transform = true;
    }

    if(!has_transform && std::filesystem::exists(files[0]+".linear_reg.txt"))
    {
        tipl::out() << "loading existing linear registration." << std::endl;
        if(!(load_mapping((files[0]+".linear_reg.txt").c_str())))
        {
            tipl::out() << "ERROR: invalid slice mapping file format" << std::endl;
            return false;
        }
        has_transform = true;
    }

    // handle registration
    if(!has_transform)
    {
        tipl::out() << "running slice registration..." << std::endl;
        if(tipl::show_prog)
        {
            thread.reset(new std::thread([this](){argmin();}));
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        else
            argmin();
    }
    else
    {
        handle->view_item.back().T = to_dif;
        handle->view_item.back().iT = to_slice;
    }
    return true;
}
void CustomSliceModel::update_image(void)
{
    dim = source_images.shape();
    slice_pos[0] = source_images.width() >> 1;
    slice_pos[1] = source_images.height() >> 1;
    slice_pos[2] = source_images.depth() >> 1;
}
// ---------------------------------------------------------------------------
void CustomSliceModel::update_transform(void)
{
    tipl::transformation_matrix<float> M(arg_min,dim,vs,handle->dim,handle->vs);
    to_dif.identity();
    M.save_to_transform(to_dif.begin());
    to_slice = tipl::inverse(to_dif);
    if(view_id)
    {
        handle->view_item[view_id].T = to_dif;
        handle->view_item[view_id].iT = to_slice;
    }
}
// ---------------------------------------------------------------------------
void match_template_resolution(tipl::image<3>& VG,
                               tipl::vector<3>& VGvs,
                               tipl::image<3>& VF,
                               tipl::vector<3>& VFvs);
void CustomSliceModel::argmin(void)
{
    terminated = false;
    running = true;
    handle->view_item[view_id].registering = true;

    auto to = source_images;
    auto to_vs = vs;
    tipl::image<3> from;
    handle->get_iso_fa(from);
    auto from_vs = handle->vs;
    match_template_resolution(to,to_vs,from,from_vs);

    tipl::lower_threshold(to,0.0f);
    tipl::lower_threshold(from,0.0f);

    tipl::filter::gaussian(to);
    tipl::filter::gaussian(from);

    tipl::out() << "registration started";
    linear_with_mi(to,to_vs,from,from_vs,arg_min,tipl::reg::rigid_body,terminated);
    update_transform();
    handle->view_item[view_id].registering = false;
    running = false;
    tipl::out() << "registration completed";
}
// ---------------------------------------------------------------------------
bool CustomSliceModel::save_mapping(const char* file_name)
{
    return !!(std::ofstream(file_name) << arg_min);
}
// ---------------------------------------------------------------------------
bool CustomSliceModel::load_mapping(const char* file_name)
{
    std::ifstream in(file_name);
    if(!in)
        return false;
    if(in.peek() == 't')
    {
        if(!(std::ifstream(file_name) >> arg_min))
            return false;
    }
    else
    {
        tipl::transformation_matrix<float> T;
        if(!(in >> T))
            return false;
        T.to_affine_transform(arg_min,dim,vs,handle->dim,handle->vs);
    }
    update_transform();
    is_diffusion_space = false;
    tipl::out() << arg_min << std::endl;
    tipl::out() << "to_dif: " << to_dif << std::endl;
    return true;
}

// ---------------------------------------------------------------------------
void CustomSliceModel::wait(void)
{
    if(thread.get() && thread->joinable())
        thread->join();
}
// ---------------------------------------------------------------------------
void CustomSliceModel::terminate(void)
{
    terminated = true;
    running = false;
    wait();
}
// ---------------------------------------------------------------------------
