#include <QApplication>
#include <QFileInfo>
#include "tracking/region/Regions.h"
#include "tracking/atlasdialog.h"
#include "tracking/roi.hpp"
#include "connectometry/group_connectometry.hpp"
#include "ui_group_connectometry.h"
#include "program_option.hpp"
bool load_roi(program_option& po,std::shared_ptr<fib_data> handle,std::shared_ptr<RoiMgr> roi_mgr);
int cnt(program_option& po)
{
    std::shared_ptr<group_connectometry_analysis> vbc(new group_connectometry_analysis);
    if(!vbc->load_database(po.get("source").c_str()))
    {
        show_progress() << "ERROR:" << vbc->error_msg << std::endl;
        return 1;
    }

    if(!po.check("demo") || !po.has("voi") || !po.has("variable_list"))
        return 1;

    // read demographic file
    auto& db = vbc->handle->db;
    if(!db.parse_demo(po.get("demo")))
    {
        show_progress() << "ERROR: " << db.error_msg << std::endl;
        return 1;
    }

    {
        show_progress pout;
        pout << "selectable variables include ";
        // show features readed
        for(size_t i = 0;i < db.feature_titles.size();++i)
             pout << "\t(" << i << ")" << db.feature_titles[i];
        pout << std::endl;
    }

    std::vector<unsigned int> variable_list;
    std::string foi_str;


    {

        std::string var_text = po.get("variable_list");
        std::replace(var_text.begin(),var_text.end(),',',' ');
        std::istringstream var_in(var_text);
        variable_list.assign((std::istream_iterator<int>(var_in)),(std::istream_iterator<int>()));
        for(auto v : variable_list)
            if(v >= db.feature_titles.size())
            {
                show_progress() << "invalid variable value: " << v << std::endl;
                return 1;
            }


        if(po.get("voi") == "Intercept")
        {
            if(!db.is_longitudinal)
            {
                show_progress() << "[ERROR] Intercept can only be studied in a longitudinal database." << std::endl;
                return 1;
            }
            foi_str = "Intercept";
        }
        else
        {
            unsigned int voi_index = po.get("voi",uint32_t(0));
            if(voi_index >= db.feature_titles.size())
            {
                show_progress() << "invalid variable of interest: " << voi_index << std::endl;
                return 1;
            }
            // the variable to study needs to be included in the model
            variable_list.push_back(voi_index);
            foi_str = db.feature_titles[voi_index];
        }

        {
            // sort and variables, make them unique
            std::set<unsigned int> s(variable_list.begin(),variable_list.end());
            variable_list.assign(s.begin(),s.end());
        }

        std::fill(db.feature_selected.begin(),db.feature_selected.end(),false);
        show_progress pout;
        pout << "variable(s) to be considered in regression: ";
        for(auto index : variable_list)
        {
            pout << "\t(" << index << ")" << db.feature_titles[index];
            db.feature_selected[index] = true;
        }
        pout << std::endl;
        pout << "variable to study: " << foi_str << std::endl;

    }



    {
        progress prog("connectometry parameters:");
        vbc->no_tractogram = (po.get("no_tractogram",1) == 1);
        vbc->foi_str = foi_str;
        vbc->length_threshold_voxels = po.get("length_threshold",uint32_t(20));
        vbc->tip = po.get("tip",uint32_t(4));
        vbc->fdr_threshold = po.get("fdr_threshold",0.0f);
        vbc->tracking_threshold = po.get("t_threshold",2.5f);

        // select cohort and feature
        vbc->model.reset(new stat_model);
        vbc->model->read_demo(db);
        vbc->model->nonparametric = po.get("nonparametric",1);
        if(!vbc->model->select_cohort(db,po.get("select")) || !vbc->model->select_feature(db,vbc->foi_str))
        {
            show_progress() << "ERROR:" << vbc->model->error_msg.c_str() << std::endl;
            return 1;
        }

        // setup roi
        vbc->roi_mgr = std::make_shared<RoiMgr>(vbc->handle);
        if(po.get("exclude_cb",1))
            vbc->exclude_cerebellum();

        if(!load_roi(po,vbc->handle,vbc->roi_mgr))
            return 1;

        // if no seed assigned, assign whole brain
        if(vbc->roi_mgr->seeds.empty())
            vbc->roi_mgr->setWholeBrainSeed(vbc->fiber_threshold);

    }



    {
        progress prog("running connectometry");
        vbc->output_file_name = po.get("output",po.get("demo")+"."+vbc->get_file_post_fix());
        vbc->run_permutation(std::thread::hardware_concurrency(),po.get("permutation",uint32_t(2000)));
        vbc->wait();
    }
    vbc->save_result();
    vbc->calculate_FDR();
    std::string output;
    vbc->generate_report(output);
    {
        std::string report_file_name = vbc->output_file_name+".report.html";
        std::ofstream out(report_file_name.c_str());
        if(!out)
            show_progress() << "cannot output file to " << report_file_name << std::endl;
        else
        {
            out << output << std::endl;
            show_progress() << "report saved to " << report_file_name << std::endl;
        }
    }
    return 0;
}
