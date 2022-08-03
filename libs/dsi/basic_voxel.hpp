#ifndef BASIC_VOXEL_HPP
#define BASIC_VOXEL_HPP
#include <string>
#include "TIPL/tipl.hpp"
#include "tessellated_icosahedron.hpp"
#include "gzip_interface.hpp"
#include "prog_interface_static_link.h"

struct ImageModel;
struct VoxelParam;
class Voxel;
struct VoxelData;
struct HistData;
class BaseProcess
{
public:
    BaseProcess(void) {}
    virtual void init(Voxel&) {}
    virtual void run(Voxel&, VoxelData&) {}
    virtual void run_hist(Voxel&,HistData&) {}
    virtual void end(Voxel&,gz_mat_write&) {}
    virtual ~BaseProcess(void) {}
};



struct VoxelData
{
    size_t voxel_index;
    std::vector<float> space;
    std::vector<float> odf;
    std::vector<float> odf1,odf2;
    std::vector<float> fa;
    std::vector<float> rdi;
    std::vector<tipl::vector<3,float> > dir;
    std::vector<unsigned short> dir_index;
    float min_odf;
    tipl::matrix<3,3,float> jacobian;
    tipl::matrix<3,3,float> grad_dev;

    void init(void)
    {
        std::fill(fa.begin(),fa.end(),0.0);
        std::fill(dir_index.begin(),dir_index.end(),0);
        std::fill(dir.begin(),dir.end(),tipl::vector<3,float>());
    }
};

struct HistData
{
public:
    tipl::image<2,unsigned char> I,I_mask;
    tipl::vector<2,int> from,to;
public:
    enum {dx = 0,dy = 1,dxx = 2,dyy = 3,dxy = 4};
    std::vector<tipl::image<2> > other_maps;
public:
    void init(void)
    {
        I.clear();
        I_mask.clear();
        other_maps.clear();
        other_maps.resize(10);
    }
};

struct ImageModel;
class Voxel
{
private:
    std::vector<std::shared_ptr<BaseProcess> > process_list;
public:
    tipl::shape<3> dim;
    tipl::vector<3> vs;
public:

    tipl::image<3,unsigned char> mask;
public:
    std::vector<const unsigned short*> dwi_data;
    std::vector<tipl::vector<3,float> > bvectors;
    std::vector<float> bvalues;

    std::string report,steps;
    std::ostringstream recon_report, step_report;
    unsigned int thread_count = std::thread::hardware_concurrency();
    void load_from_src(ImageModel& image_model);
public:
    unsigned char method_id;
    float param[3] = {1.25f,3000.0f,0.05f};
    tessellated_icosahedron ti;
public:
    std::string file_name;
    bool output_odf = false;
    bool check_btable = true;
    unsigned int max_fiber_number = 5;
    std::vector<std::string> file_list;
public:
    std::string other_output;
    bool needs(const char* metric)
    {
        if(other_output == "all" && std::string(metric) != "debug")
            return true;
        std::istringstream in(other_output);
        std::string m;
        while(std::getline(in,m,','))
            if(m == metric)
                return true;
        return false;
    }
public:
    tipl::image<2,unsigned char> hist_image;
    unsigned int hist_downsampling = 4;
    unsigned int hist_raw_smoothing = 4;
    unsigned int hist_tensor_smoothing = 8;
    bool is_histology = false;
    unsigned int crop_size = 1024;
    unsigned int margin = 128;
public:// DTI
    bool dti_no_high_b = true;
public://used in GQI
    bool odf_resolving = false;
    bool r2_weighted = false;// used in GQI only
    bool half_sphere = true;
public://used in GQI
    bool scheme_balance = false;
public:// used in QSDR
    tipl::matrix<4,4> trans_to_mni;
    size_t template_id = 0;
    tipl::transformation_matrix<float> qsdr_trans;
    bool qsdr = false;
    tipl::vector<3,int> csf_pos1,csf_pos2,csf_pos3,csf_pos4;
    float R2;
    float qsdr_reso = 1.0f;
public: // for QSDR associated T1WT2W
    std::vector<tipl::image<3> > other_image;
    std::vector<std::string> other_image_name;
    std::vector<tipl::transformation_matrix<float> > other_image_trans;

public: // for fib evaluation
    tipl::image<3> fib_fa;
    std::vector<tipl::vector<3> > fib_dir;
public:
    float z0 = 1.0f;
    // other information for second pass processing
    std::vector<float> response_function,free_water_diffusion;
    tipl::image<3> qa_map,iso_map;
    float reponse_function_scaling;
public:// for template creation
    std::vector<std::vector<float> > template_odfs;
    std::vector<tipl::image<3> > template_metrics;
    std::vector<std::string> template_metrics_name;
    std::string template_file_name;
public:
    std::vector<VoxelData> voxel_data;
    std::vector<HistData> hist_data;
public:
    template<typename T,typename ...Ts>
    void add_process(void)
    {
        process_list.push_back(std::make_shared<T>());
        if constexpr (sizeof...(Ts) > 0) {
            add_process<Ts...>();
        }
    }
    template<typename ...Ts>
    void init_process(void)
    {
        process_list.clear();
        add_process<Ts...>();
        init();
    }
public:
    void init(void);
    bool run(void);
    bool run_hist(void);
    void end(gz_mat_write& writer);
    BaseProcess* get(unsigned int index);
};




#endif//BASIC_VOXEL_HPP
