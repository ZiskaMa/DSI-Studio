#include <QSplitter>
#include <QThread>
#include "reconstruction_window.h"
#include "ui_reconstruction_window.h"
#include "TIPL/tipl.hpp"
#include "reg.hpp"
#include "mainwindow.h"
#include <QImage>
#include <QMessageBox>
#include <QInputDialog>
#include <QFileDialog>
#include <QSettings>
#include "prog_interface_static_link.h"
#include "tracking/region/Regions.h"
#include "libs/dsi/image_model.hpp"
#include "gzip_interface.hpp"
#include "manual_alignment.h"

#include <filesystem>


void show_view(QGraphicsScene& scene,QImage I);
void populate_templates(QComboBox* combo,size_t index);
bool reconstruction_window::load_src(int index)
{
    progress prog_("read SRC file");
    handle = std::make_shared<ImageModel>();
    if (!handle->load_from_file(filenames[index].toLocal8Bit().begin()))
        return false;
    if(handle->voxel.is_histology)
        return true;
    auto I = tipl::make_image(handle->src_dwi_data[0],handle->voxel.dim);
    double m = double(tipl::max_value(I));
    double otsu = double(tipl::segmentation::otsu_threshold(I));
    ui->max_value->setMaximum(m*1.5);
    ui->max_value->setMinimum(0.0);
    ui->max_value->setSingleStep(m*0.05);
    ui->max_value->setValue(otsu*3.0);
    ui->min_value->setMaximum(m*1.5);
    ui->min_value->setMinimum(0.0);
    ui->min_value->setSingleStep(m*0.05);
    ui->min_value->setValue(0.0);
    load_b_table();

    ui->align_slices->setVisible(false);
    return true;
}

void calculate_shell(const std::vector<float>& bvalues,std::vector<unsigned int>& shell);
bool is_dsi_half_sphere(const std::vector<unsigned int>& shell);
bool is_dsi(const std::vector<unsigned int>& shell);
bool is_multishell(const std::vector<unsigned int>& shell);
bool need_scheme_balance(const std::vector<unsigned int>& shell);
extern std::vector<std::string> fa_template_list,iso_template_list;
reconstruction_window::reconstruction_window(QStringList filenames_,QWidget *parent) :
    QMainWindow(parent),filenames(filenames_),ui(new Ui::reconstruction_window)
{
    ui->setupUi(this);
    if(!load_src(0))
        throw std::runtime_error("Cannot load src file");
    setWindowTitle(filenames[0]);
    ui->ThreadCount->setMaximum(std::thread::hardware_concurrency());
    ui->toolBox->setCurrentIndex(1);
    ui->graphicsView->setScene(&scene);
    ui->view_source->setScene(&source);
    ui->b_table->setColumnWidth(0,60);
    ui->b_table->setColumnWidth(1,80);
    ui->b_table->setColumnWidth(2,80);
    ui->b_table->setColumnWidth(3,80);
    ui->b_table->setHorizontalHeaderLabels(QStringList() << "b value" << "bx" << "by" << "bz");

    populate_templates(ui->primary_template,handle->voxel.template_id);
    if(ui->primary_template->currentIndex() == 0)
        ui->diffusion_sampling->setValue(1.25); // human studies
    else
        ui->diffusion_sampling->setValue(0.6);  // animal studies (likely ex-vivo)

    v2c.two_color(tipl::rgb(0,0,0),tipl::rgb(255,255,255));
    update_dimension();

    absolute_path = QFileInfo(filenames[0]).absolutePath();


    switch(settings.value("rec_method_id",4).toInt())
    {
    case 1:
        ui->DTI->setChecked(true);
        on_DTI_toggled(true);
        break;
    case 7:
        ui->QSDR->setChecked(true);
        on_QSDR_toggled(true);
        break;
    default:
        ui->GQI->setChecked(true);
        on_GQI_toggled(true);
        break;
    }


    ui->align_acpc->setChecked(handle->is_human_data() &&
                               handle->voxel.vs[0] == handle->voxel.vs[2]);
    ui->odf_resolving->setVisible(false);

    ui->AdvancedWidget->setVisible(false);
    ui->ThreadCount->setValue(settings.value("rec_thread_count",std::thread::hardware_concurrency()).toInt());

    ui->odf_resolving->setChecked(settings.value("odf_resolving",0).toInt());

    ui->RecordODF->setChecked(settings.value("rec_record_odf",0).toInt());

    ui->check_btable->setChecked(settings.value("check_btable",1).toInt());
    if(handle->voxel.vs[2] > handle->voxel.vs[0]*2.0f || handle->voxel.vs[0] < 0.5f)
        ui->check_btable->setChecked(false);
    ui->report->setText(handle->voxel.report.c_str());
    ui->dti_no_high_b->setChecked(handle->is_human_data());

    ui->method_group->setVisible(!handle->voxel.is_histology);
    ui->param_group->setVisible(!handle->voxel.is_histology);
    ui->hist_param_group->setVisible(handle->voxel.is_histology);

    ui->qsdr_reso->setValue(handle->voxel.vs[0]);

    if(handle->voxel.is_histology)
    {
        delete ui->menuCorrections;
        delete ui->menuB_table;
        delete ui->menuFile;
        ui->source_page->hide();
        ui->toolBox->removeItem(0);
        auto actions = ui->menuEdit->actions();
        for(int i = 4;i < actions.size();++i)
            actions[i]->setVisible(false);

        ui->hist_downsampling->setValue(std::ceil(std::log2(handle->voxel.hist_image.width()))-12);
    }


    connect(ui->z_pos,SIGNAL(valueChanged(int)),this,SLOT(on_b_table_itemSelectionChanged()));
    connect(ui->max_value,SIGNAL(valueChanged(double)),this,SLOT(on_b_table_itemSelectionChanged()));
    connect(ui->min_value,SIGNAL(valueChanged(double)),this,SLOT(on_b_table_itemSelectionChanged()));

    on_b_table_itemSelectionChanged();


    {
        ui->scheme_balance->setChecked(handle->need_scheme_balance());
        if(handle->is_dsi())
            ui->scheme_balance->setEnabled(false);
    }

    ui->mask_edit->setVisible(false);

}
void reconstruction_window::update_dimension(void)
{
    if(ui->SlicePos->maximum() != handle->voxel.dim[2]-1)
    {
        ui->SlicePos->setRange(0,handle->voxel.dim[2]-1);
        ui->SlicePos->setValue((handle->voxel.dim[2]-1) >> 1);
    }
    if(ui->z_pos->maximum() != handle->voxel.dim[view_orientation]-1)
    {
        ui->z_pos->setRange(0,handle->voxel.dim[view_orientation]-1);
        ui->z_pos->setValue((handle->voxel.dim[view_orientation]-1) >> 1);
    }
    source_ratio = std::max(1.0,500/(double)handle->voxel.dim.height());
}

void reconstruction_window::load_b_table(void)
{
    if(handle->src_bvalues.empty())
        return;
    ui->b_table->clear();
    ui->b_table->setRowCount(handle->src_bvalues.size());
    for(unsigned int index = 0;index < handle->src_bvalues.size();++index)
    {
        ui->b_table->setItem(index,0, new QTableWidgetItem(QString::number(handle->src_bvalues[index])));
        ui->b_table->setItem(index,1, new QTableWidgetItem(QString::number(handle->src_bvectors[index][0])));
        ui->b_table->setItem(index,2, new QTableWidgetItem(QString::number(handle->src_bvectors[index][1])));
        ui->b_table->setItem(index,3, new QTableWidgetItem(QString::number(handle->src_bvectors[index][2])));
    }
    ui->b_table->selectRow(0);
}

void reconstruction_window::on_b_table_itemSelectionChanged()
{
    if(handle->src_bvalues.empty())
        return;
    v2c.set_range(ui->min_value->value(),ui->max_value->value());
    tipl::image<2,float> tmp;
    tipl::volume2slice(tipl::make_image(handle->src_dwi_data[ui->b_table->currentRow()],handle->voxel.dim),tmp,view_orientation,ui->z_pos->value());
    buffer_source.resize(tmp.shape());
    for(int i = 0;i < tmp.size();++i)
        buffer_source[i] = v2c[tmp[i]];

    // show bad_slices
    if(view_orientation != 2 && bad_slice_analzed)
    {
        std::vector<size_t> mark_slices;
        for(size_t index = 0;index < bad_slices.size();++index)
            if(bad_slices[index].first == ui->b_table->currentRow())
                mark_slices.push_back(bad_slices[index].second);
        for(size_t index = 0;index < mark_slices.size();++index)
        {
            for(size_t x = 0,pos = mark_slices[index]*buffer_source.width();x < buffer_source.width();++x,++pos)
                buffer_source[pos].r |= 64;
        }
    }
    if(view_orientation == 2 && bad_slice_analzed)
    {
        std::vector<size_t> mark_slices;
        for(size_t index = 0;index < bad_slices.size();++index)
            if(bad_slices[index].first == ui->b_table->currentRow() && ui->z_pos->value() == bad_slices[index].second)
            {
                for(size_t i = 0;i < buffer_source.size();++i)
                    buffer_source[i].r |= 64;
                break;
            }
    }

    source_image = QImage((unsigned char*)&*buffer_source.begin(),tmp.width(),tmp.height(),QImage::Format_RGB32).
                    scaled(tmp.width()*source_ratio,tmp.height()*source_ratio);

    if(view_orientation != 2)
        source_image = source_image.mirrored();
    show_view(source,source_image);
}


void reconstruction_window::resizeEvent ( QResizeEvent * event )
{
    QMainWindow::resizeEvent(event);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}
void reconstruction_window::showEvent ( QShowEvent * event )
{
    QMainWindow::showEvent(event);
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::closeEvent(QCloseEvent *event)
{
    QMainWindow::closeEvent(event);

}

reconstruction_window::~reconstruction_window()
{
    delete ui;
}

void reconstruction_window::Reconstruction(unsigned char method_id,bool prompt)
{
    progress prog_("reconstruction");
    if(!handle.get())
        return;

    if (tipl::max_value(handle->voxel.mask) == 0)
    {
        QMessageBox::information(this,"error","Please select mask for reconstruction");
        return;
    }

    settings.setValue("rec_method_id",method_id);
    settings.setValue("rec_thread_count",ui->ThreadCount->value());

    settings.setValue("odf_resolving",ui->odf_resolving->isChecked() ? 1 : 0);
    settings.setValue("rec_record_odf",ui->RecordODF->isChecked() ? 1 : 0);
    settings.setValue("other_output",ui->other_output->text());
    settings.setValue("check_btable",ui->check_btable->isChecked() ? 1 : 0);

    handle->voxel.method_id = method_id;
    handle->voxel.ti.init(8);
    handle->voxel.odf_resolving = ui->odf_resolving->isChecked();
    handle->voxel.output_odf = ui->RecordODF->isChecked();
    handle->voxel.dti_no_high_b = ui->dti_no_high_b->isChecked();
    handle->voxel.check_btable = ui->check_btable->isChecked();
    handle->voxel.other_output = ui->other_output->text().toStdString();
    handle->voxel.thread_count = ui->ThreadCount->value();
    handle->voxel.template_id = ui->primary_template->currentIndex();

    if(handle->voxel.is_histology)
    {
        handle->voxel.vs[0] = handle->voxel.vs[1] = handle->voxel.vs[2] = float(ui->hist_resolution->value());
        handle->voxel.hist_downsampling = uint32_t(ui->hist_downsampling->value());
        handle->voxel.hist_raw_smoothing = uint32_t(ui->hist_raw_smoothing->value());
        handle->voxel.hist_tensor_smoothing = uint32_t(ui->hist_tensor_smoothing->value());
    }

    if(method_id == 7 || method_id == 4)
        handle->voxel.scheme_balance = ui->scheme_balance->isChecked() ? 1:0;
    else
        handle->voxel.scheme_balance = false;

    if(method_id == 7)
    {
        if(fa_template_list.empty())
        {
            QMessageBox::information(this,"error","Cannot find template files");
            return;
        }
        handle->voxel.qsdr_reso = ui->qsdr_reso->value();
    }
    else
        if(ui->align_acpc->isChecked())
            handle->align_acpc();

    auto dim_backup = handle->voxel.dim; // for QSDR
    auto vs = handle->voxel.vs; // for QSDR
    if (!handle->reconstruction())
    {
        QMessageBox::critical(this,"ERROR",handle->error_msg.c_str());
        return;
    }
    handle->voxel.dim = dim_backup;
    handle->voxel.vs = vs;
    if(method_id == 7) // QSDR
        handle->calculate_dwi_sum(true);
    if(!prompt)
        return;
    QMessageBox::information(this,"DSI Studio","FIB file created.");
    raise(); // for Mac
    QString filename = handle->file_name.c_str();
    filename += handle->get_file_ext().c_str();
    if(method_id == 6)
        ((MainWindow*)parent())->addSrc(filename);
    else
        ((MainWindow*)parent())->addFib(filename);
}

void reconstruction_window::on_load_mask_clicked()
{
    QString filename = QFileDialog::getOpenFileName(
            this,
            "Open region",
            absolute_path,
            "Mask files (*.nii *nii.gz *.hdr);;Text files (*.txt);;All files (*)" );
    if(filename.isEmpty())
        return;
    command("[Step T2a][Open]",filename.toStdString());
    on_SlicePos_valueChanged(ui->SlicePos->value());
}


void reconstruction_window::on_save_mask_clicked()
{
    QString filename = QFileDialog::getSaveFileName(
            this,
            "Save region",
            absolute_path+"/mask.nii.gz",
            "Nifti file(*nii.gz *.nii);;Text files (*.txt);;All files (*)" );
    if(filename.isEmpty())
        return;
    if(QFileInfo(filename.toLower()).completeSuffix() != "txt")
        filename = QFileInfo(filename).absolutePath() + "/" + QFileInfo(filename).baseName() + ".nii.gz";
    ROIRegion region(handle->dwi.shape(),handle->voxel.vs);
    region.LoadFromBuffer(handle->voxel.mask);
    region.SaveToFile(filename.toLocal8Bit().begin());
}
void reconstruction_window::on_actionFlip_bx_triggered()
{
    command("[Step T2][B-table][flip bx]");
    ui->check_btable->setChecked(false);
    QMessageBox::information(this,"DSI Studio","B-table flipped");
}
void reconstruction_window::on_actionFlip_by_triggered()
{
    command("[Step T2][B-table][flip by]");
    ui->check_btable->setChecked(false);
    QMessageBox::information(this,"DSI Studio","B-table flipped");
}
void reconstruction_window::on_actionFlip_bz_triggered()
{
    command("[Step T2][B-table][flip bz]");
    ui->check_btable->setChecked(false);
    QMessageBox::information(this,"DSI Studio","B-table flipped");
}
void reconstruction_window::batch_command(std::string cmd,std::string param)
{
    command(cmd,param);
    if(filenames.size() > 1 && QMessageBox::information(this,"DSI Studio","Apply to other SRC files?",
                                    QMessageBox::Yes|QMessageBox::No|QMessageBox::Cancel) == QMessageBox::Yes)
    {
        progress prog_("apply to other SRC files");
        auto steps = handle->voxel.steps;
        steps += cmd;
        if(!param.empty())
        {
            steps += "=";
            steps += param;
            steps += "\n";
        }
        for(int index = 1;progress::at(index,filenames.size());++index)
        {
            ImageModel model;
            if (!model.load_from_file(filenames[index].toStdString().c_str()) ||
                !model.run_steps(handle->file_name,steps))
            {
                QMessageBox::critical(this,"ERROR",QFileInfo(filenames[index]).fileName() + " : " + model.error_msg.c_str());
                return;
            }
        }
    }
}
bool reconstruction_window::command(std::string cmd,std::string param)
{
    bool result = handle->command(cmd,param);
    if(!result)
        QMessageBox::critical(this,"ERROR",handle->error_msg.c_str());
    update_dimension();
    load_b_table();
    on_SlicePos_valueChanged(ui->SlicePos->value());
    return result;
}
void reconstruction_window::on_doDTI_clicked()
{
    std::string ref_file_name = handle->file_name;
    std::string ref_steps = handle->voxel.steps;
    std::shared_ptr<ImageModel> ref_handle = handle;
    progress prog_("SRC files");
    for(int index = 0;progress::at(index,filenames.size());++index)
    {
        if(index)
        {
            if(!load_src(index) || !handle->run_steps(ref_file_name,ref_steps))
            {
                if(!progress::aborted())
                    QMessageBox::critical(this,"ERROR",QFileInfo(filenames[index]).fileName() + " : " + handle->error_msg.c_str());
                break;
            }
        }
        if(ui->DTI->isChecked())
            Reconstruction(1,index+1 == filenames.size());
        else
        if(ui->GQI->isChecked() || ui->QSDR->isChecked())
        {
            handle->voxel.param[0] = float(ui->diffusion_sampling->value());
            settings.setValue("rec_gqi_sampling",ui->diffusion_sampling->value());
            if(ui->QSDR->isChecked())
                Reconstruction(7,index+1 == filenames.size());
            else
                Reconstruction(4,index+1 == filenames.size());
        }
    }
    handle = ref_handle;
}

void reconstruction_window::on_DTI_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(!checked);
    ui->GQIOption_2->setVisible(!checked);

    ui->AdvancedOptions->setVisible(checked);

    ui->RecordODF->setVisible(!checked);
    ui->qsdr_reso->setVisible(!checked);
    ui->qsdr_reso_label->setVisible(!checked);
    if(checked && (!ui->other_output->text().contains("rd") &&
                   !ui->other_output->text().contains("ad") &&
                   !ui->other_output->text().contains("md")))
        ui->other_output->setText("fa,rd,ad,md");

}


void reconstruction_window::on_GQI_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(!checked);


    ui->GQIOption_2->setVisible(checked);

    ui->AdvancedOptions->setVisible(checked);

    ui->RecordODF->setVisible(checked);

    ui->qsdr_reso->setVisible(!checked);
    ui->qsdr_reso_label->setVisible(!checked);
}

void reconstruction_window::on_QSDR_toggled(bool checked)
{
    ui->ResolutionBox->setVisible(checked);
    ui->GQIOption_2->setVisible(checked);

    ui->AdvancedOptions->setVisible(checked);

    ui->RecordODF->setVisible(checked);

    ui->qsdr_reso->setVisible(checked);
    ui->qsdr_reso_label->setVisible(checked);

}

void reconstruction_window::on_zoom_in_clicked()
{
    source_ratio *= 1.1f;
    on_b_table_itemSelectionChanged();
}

void reconstruction_window::on_zoom_out_clicked()
{
    source_ratio *= 0.9f;
    on_b_table_itemSelectionChanged();
}

void reconstruction_window::on_AdvancedOptions_clicked()
{
    if(ui->AdvancedOptions->text() == "Advanced Options >>")
    {
        ui->AdvancedWidget->setVisible(true);
        ui->AdvancedOptions->setText("Advanced Options <<");
    }
    else
    {
        ui->AdvancedWidget->setVisible(false);
        ui->AdvancedOptions->setText("Advanced Options >>");
    }
}


void reconstruction_window::on_actionSave_4D_nifti_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save image as...",
                            filenames[0] + ".nii.gz",
                                "All files (*)" );
    if ( filename.isEmpty() )
        return;

    batch_command("[Step T2][File][Save 4D NIFTI]",filename.toStdString());

}

void reconstruction_window::on_actionSave_b0_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save image as...",
                            filenames[0] + ".b0.nii.gz",
                                "All files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_b0_to_nii(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_DWI_sum_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save image as...",
                            filenames[0] + ".dwi_sum.nii.gz",
                                "All files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_dwi_sum_to_nii(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_b_table_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save b table as...",
                            QFileInfo(filenames[0]).absolutePath() + "/b_table.txt",
                                "Text files (*.txt)" );
    if ( filename.isEmpty() )
        return;
    handle->save_b_table(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_bvals_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save b table as...",
                                QFileInfo(filenames[0]).absolutePath() + "/bvals",
                                "Text files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_bval(filename.toLocal8Bit().begin());
}

void reconstruction_window::on_actionSave_bvecs_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
                                this,
                                "Save b table as...",
                                QFileInfo(filenames[0]).absolutePath() + "/bvecs",
                                "Text files (*)" );
    if ( filename.isEmpty() )
        return;
    handle->save_bvec(filename.toLocal8Bit().begin());
}


bool load_image_from_files(QStringList filenames,tipl::image<3>& ref,tipl::vector<3>& vs,tipl::matrix<4,4>&);
void reconstruction_window::on_actionRotate_triggered()
{
    QStringList filenames = QFileDialog::getOpenFileNames(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz *.dcm);;All files (*)" );
    if( filenames.isEmpty())
        return;

    tipl::image<3> ref;
    tipl::vector<3> vs;
    tipl::matrix<4,4> t;
    if(!load_image_from_files(filenames,ref,vs,t))
        return;
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,
                                                                handle->dwi,handle->voxel.vs,ref,vs,
                                                                tipl::reg::rigid_body,
                                                                tipl::reg::cost_type::mutual_info));
    manual->on_rerun_clicked();
    if(manual->exec() != QDialog::Accepted)
        return;

    progress prog_("rotating");
    tipl::image<3> ref2(ref);
    float m = tipl::median(ref2.begin(),ref2.end());
    tipl::multiply_constant_mt(ref,0.5f/m);
    handle->rotate(ref.shape(),vs,manual->get_iT());
    handle->voxel.report += " The diffusion images were rotated and scaled to the space of ";
    handle->voxel.report += QFileInfo(filenames[0]).baseName().toStdString();
    handle->voxel.report += ". The b-table was also rotated accordingly.";
    ui->report->setText(handle->voxel.report.c_str());
    load_b_table();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());

}


void reconstruction_window::on_delete_2_clicked()
{
    if(handle->src_dwi_data.size() == 1)
        return;
    int index = ui->b_table->currentRow();
    if(index < 0)
        return;
    bad_slice_analzed = false;
    ui->b_table->removeRow(index);
    handle->remove(uint32_t(index));

}

void reconstruction_window::on_remove_below_clicked()
{
    if(handle->src_dwi_data.size() == 1)
        return;
    int index = ui->b_table->currentRow();
    if(index <= 0)
        return;
    bad_slice_analzed = false;
    while(ui->b_table->rowCount() > index)
    {
        ui->b_table->removeRow(index);
        handle->remove(uint32_t(index));
    }
}


void reconstruction_window::on_SlicePos_valueChanged(int position)
{
    handle->draw_mask(buffer,position);
    double ratio =
        std::min(double(ui->graphicsView->width()-5)/double(buffer.width()),
                 double(ui->graphicsView->height()-5)/double(buffer.height()));
    slice_image = QImage(reinterpret_cast<unsigned char*>(&*buffer.begin()),
                         buffer.width(),buffer.height(),QImage::Format_RGB32).
                            scaled(int(buffer.width()*ratio),int(buffer.height()*ratio));
    show_view(scene,slice_image);
}

bool add_other_image(ImageModel* handle,QString name,QString filename)
{
    tipl::image<3> ref;
    tipl::vector<3> vs;
    gz_nifti in;
    if(!in.load_from_file(filename.toLocal8Bit().begin()) || !in.toLPS(ref))
    {
        std::cout << "not a valid nifti file:" << filename.toStdString() << std::endl;
        return false;
    }

    std::cout << "add " << filename.toStdString() << " as " << name.toStdString();

    tipl::transformation_matrix<float> affine;
    bool has_registered = false;
    for(unsigned int index = 0;index < handle->voxel.other_image.size();++index)
        if(ref.shape() == handle->voxel.other_image[index].shape())
        {
            affine = handle->voxel.other_image_trans[index];
            has_registered = true;
        }
    if(!has_registered && ref.shape() != handle->voxel.dim)
    {
        std::cout << " and register image with DWI." << std::endl;
        in.get_voxel_size(vs);
        tipl::image<3> iso_fa(handle->dwi);
        bool terminated = false;
        auto smoothed_ref = ref;
        tipl::filter::gaussian(iso_fa);
        tipl::filter::gaussian(iso_fa);
        tipl::filter::gaussian(smoothed_ref);
        tipl::filter::gaussian(smoothed_ref);
        linear_with_mi(iso_fa,handle->voxel.vs,smoothed_ref,vs,affine,tipl::reg::rigid_body,terminated);
    }
    else {
        if(has_registered)
            std::cout << " using previous registration." << std::endl;
        else
            std::cout << " treated as DWI space images." << std::endl;
    }
    handle->voxel.other_image.push_back(std::move(ref));
    handle->voxel.other_image_name.push_back(name.toStdString());
    handle->voxel.other_image_trans.push_back(affine);
    return true;
}

void reconstruction_window::on_add_t1t2_clicked()
{
    QString filename = QFileDialog::getOpenFileName(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz);;All files (*)" );
    if( filename.isEmpty())
        return;
    if(add_other_image(handle.get(),QFileInfo(filename).baseName(),filename))
        QMessageBox::information(this,"DSI Studio","File added");
    else
        QMessageBox::critical(this,"ERROR","Not a valid nifti file");

}

void reconstruction_window::on_actionManual_Rotation_triggered()
{
    std::shared_ptr<manual_alignment> manual(
                new manual_alignment(this,handle->dwi,handle->voxel.vs,handle->dwi,handle->voxel.vs,tipl::reg::rigid_body,tipl::reg::cost_type::mutual_info));
    if(manual->exec() != QDialog::Accepted)
        return;
    progress prog_("rotating");
    handle->rotate(handle->dwi.shape(),handle->voxel.vs,manual->get_iT());
    load_b_table();
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}



void reconstruction_window::on_actionReplace_b0_by_T2W_image_triggered()
{
    QString filename = QFileDialog::getOpenFileName(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz);;All files (*)" );
    if( filename.isEmpty())
        return;
    tipl::image<3> ref;
    tipl::vector<3> vs;
    gz_nifti in;
    if(!in.load_from_file(filename.toLocal8Bit().begin()) || !in.toLPS(ref))
    {
        QMessageBox::critical(this,"ERROR","Not a valid nifti file");
        return;
    }
    in.get_voxel_size(vs);
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,handle->dwi,handle->voxel.vs,ref,vs,tipl::reg::rigid_body,tipl::reg::cost_type::corr));
    manual->on_rerun_clicked();
    if(manual->exec() != QDialog::Accepted)
        return;

    progress prog_("rotating");
    handle->rotate(ref.shape(),vs,manual->get_iT());
    auto I = tipl::make_image((unsigned short*)handle->src_dwi_data[0],handle->voxel.dim);
    ref *= float(tipl::max_value(I))/float(tipl::max_value(ref));
    std::copy(ref.begin(),ref.end(),I.begin());
    update_dimension();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

bool get_src(std::string filename,ImageModel& src2,std::string& error_msg)
{
    progress prog_("load ",filename.c_str());
    tipl::image<3,unsigned short> I;
    if(QString(filename.c_str()).endsWith(".dcm"))
    {
        tipl::io::dicom in;
        if(!in.load_from_file(filename.c_str()))
        {
            error_msg = "invalid dicom format";
            return false;
        }
        in >> I;
        src2.voxel.dim = I.shape();
        src2.src_dwi_data.push_back(&I[0]);
    }
    else
    if(QString(filename.c_str()).endsWith(".nii.gz") ||
       QString(filename.c_str()).endsWith(".nii"))
    {
        gz_nifti in;
        if(!in.load_from_file(filename.c_str()))
        {
            error_msg = "invalid NIFTI format";
            return false;
        }
        in.toLPS(I);
        src2.voxel.dim = I.shape();
        src2.src_dwi_data.push_back(&I[0]);
    }
    else
    {
        if (!src2.load_from_file(filename.c_str()))
        {
            error_msg = "cannot open ";
            error_msg += filename;
            error_msg += " : ";
            error_msg += src2.error_msg;
            return false;
        }
    }
    return true;
}

void reconstruction_window::on_actionCorrect_AP_PA_scans_triggered()
{
    QMessageBox::information(this,"DSI Studio","Please specify another SRC/DICOM/NIFTI file with an opposite phase encoding");
    QString filename = QFileDialog::getOpenFileName(
            this,"Open SRC file",absolute_path,
            "Images (*src.gz *.nii *nii.gz);;DICOM image (*.dcm);;All files (*)" );
    if( filename.isEmpty())
        return;

    if(!handle->distortion_correction(filename.toStdString().c_str()))
    {
        QMessageBox::critical(this,"Error",handle->error_msg.c_str());
        return;
    }
    on_SlicePos_valueChanged(ui->SlicePos->value());
}



void reconstruction_window::on_actionEnable_TEST_features_triggered()
{
    ui->odf_resolving->setVisible(true);
    ui->align_slices->setVisible(true);
}

void reconstruction_window::on_actionImage_upsample_to_T1W_TESTING_triggered()
{
    QStringList filenames = QFileDialog::getOpenFileNames(
            this,"Open Images files",absolute_path,
            "Images (*.nii *nii.gz *.dcm);;All files (*)" );
    if( filenames.isEmpty())
        return;

    tipl::image<3> ref;
    tipl::vector<3> vs;
    tipl::matrix<4,4> t;
    if(!load_image_from_files(filenames,ref,vs,t))
        return;
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,
                                                                handle->dwi,handle->voxel.vs,ref,vs,
                                                                tipl::reg::rigid_body,
                                                                tipl::reg::cost_type::mutual_info));
    manual->on_rerun_clicked();
    if(manual->exec() != QDialog::Accepted)
        return;
    progress prog_("rotating");
    handle->rotate(ref.shape(),vs,manual->get_iT(),tipl::image<3,tipl::vector<3> >());
    handle->voxel.report += " The diffusion images were rotated and scaled to the space of ";
    handle->voxel.report += QFileInfo(filenames[0]).baseName().toStdString();
    handle->voxel.report += ". The b-table was also rotated accordingly.";
    ui->report->setText(handle->voxel.report.c_str());

    update_dimension();
    load_b_table();
    on_SlicePos_valueChanged(ui->SlicePos->value());
}

void reconstruction_window::on_SagView_clicked()
{
    view_orientation = 0;
    ui->z_pos->setRange(0,handle->voxel.dim[view_orientation]-1);
    ui->z_pos->setValue((handle->voxel.dim[view_orientation]-1) >> 1);
    on_b_table_itemSelectionChanged();
}

void reconstruction_window::on_CorView_clicked()
{
    view_orientation = 1;
    ui->z_pos->setRange(0,handle->voxel.dim[view_orientation]-1);
    ui->z_pos->setValue((handle->voxel.dim[view_orientation]-1) >> 1);
    on_b_table_itemSelectionChanged();
}

void reconstruction_window::on_AxiView_clicked()
{
    view_orientation = 2;
    ui->z_pos->setRange(0,handle->voxel.dim[view_orientation]-1);
    ui->z_pos->setValue((handle->voxel.dim[view_orientation]-1) >> 1);
    on_b_table_itemSelectionChanged();
}

void reconstruction_window::on_actionResample_triggered()
{
    bool ok;
    float nv = float(QInputDialog::getDouble(this,
        "DSI Studio","Assign output resolution in (mm):", double(handle->voxel.vs[0]),0.0,3.0,4, &ok));
    if (!ok || nv == 0.0f)
        return;
    command("[Step T2][Edit][Resample]",QString::number(nv).toStdString());
}

void reconstruction_window::on_actionSave_SRC_file_as_triggered()
{
    QString filename = QFileDialog::getSaveFileName(
            this,"Save SRC file",filenames[0],
            "SRC files (*src.gz);;All files (*)" );
    if(filename.isEmpty())
        return;
    batch_command("[Step T2][File][Save Src File]",filename.toStdString());
}


void reconstruction_window::on_actionEddy_Motion_Correction_triggered()
{
    handle->correct_motion(false);
    if(!progress::aborted())
    {
        handle->calculate_dwi_sum(true);
        load_b_table();
        on_SlicePos_valueChanged(ui->SlicePos->value());
    }
}

void reconstruction_window::on_show_bad_slice_clicked()
{
    if(!bad_slice_analzed)
    {
        bad_slices = handle->get_bad_slices();
        bad_slice_analzed = true;
        std::vector<char> is_bad(ui->b_table->rowCount());
        for(int i = 0;i < bad_slices.size();++i)
            if(bad_slices[i].first < is_bad.size())
                is_bad[bad_slices[i].first] = 1;

        for(int i = 0;i < ui->b_table->rowCount();++i)
            for(int j = 0;j < ui->b_table->columnCount();++j)
                ui->b_table->item(i, j)->setData(Qt::BackgroundRole,is_bad[i] ?  QColor (255,200,200): QColor (255,255,255));
    }
    if(bad_slices.size() == 0)
    {
        QMessageBox::information(this,"DSI Studio","No bad slice found in this data");
        return;
    }
    on_b_table_itemSelectionChanged();
    ui->bad_slice_label->setText(QString("A total %1 bad slices marked by red").arg(bad_slices.size()));

}

void reconstruction_window::on_align_slices_clicked()
{
    tipl::image<3> from(handle->voxel.dim);
    tipl::image<3> to(handle->voxel.dim);
    std::copy(handle->src_dwi_data[0],handle->src_dwi_data[0]+to.size(),to.begin());
    std::copy(handle->src_dwi_data[ui->b_table->currentRow()],
              handle->src_dwi_data[ui->b_table->currentRow()]+from.size(),from.begin());
    tipl::normalize(from,1.0f);
    tipl::normalize(to,1.0f);
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,
                                                                from,handle->voxel.vs,
                                                                to,handle->voxel.vs,
                                                                tipl::reg::rigid_body,
                                                                tipl::reg::cost_type::mutual_info));
    manual->on_rerun_clicked();
    if(manual->exec() != QDialog::Accepted)
        return;

    handle->rotate_one_dwi(ui->b_table->currentRow(),manual->get_iT());

    update_dimension();
    load_b_table();
    on_SlicePos_valueChanged(ui->SlicePos->value());

}

void reconstruction_window::on_edit_mask_clicked()
{
    ui->edit_mask->setVisible(false);
    ui->mask_edit->setVisible(true);

}


void reconstruction_window::on_actionOverwrite_Voxel_Size_triggered()
{
    bool ok;
    QString result = QInputDialog::getText(this,"DSI Studio","Assign voxel size in mm",
                                           QLineEdit::Normal,
                                           QString("%1 %2 %3").arg(double(handle->voxel.vs[0]))
                                                              .arg(double(handle->voxel.vs[1]))
                                                              .arg(double(handle->voxel.vs[2])),&ok);
    if(!ok)
        return;
    command("[Step T2][Edit][Overwrite Voxel Size]",result.toStdString());
    handle->get_report(handle->voxel.report);
    ui->report->setText(handle->voxel.report.c_str());
}

void match_template_resolution(tipl::image<3>& VG,
                               tipl::image<3>& VG2,
                               tipl::vector<3>& VGvs,
                               tipl::image<3>& VF,
                               tipl::image<3>& VF2,
                               tipl::vector<3>& VFvs)
{
    float ratio = float(VF.width())/float(VG.width());
    std::cout << "width ratio (subject/template):(" << VF.width() << "/" << VG.width() << ") " << ratio << std::endl;
    while(ratio < 0.5f)   // if subject resolution is substantially lower, downsample template
    {
        tipl::downsampling(VG);
        if(!VG2.empty())
            tipl::downsampling(VG2);
        VGvs *= 2.0f;
        ratio *= 2.0f;
        std::cout << "ratio lower than 0.5, downsampling template to " << VGvs[0] << " mm resolution" << std::endl;
    }
    while(ratio > 2.5f)  // if subject resolution is higher, downsample it for registration
    {
        tipl::downsampling(VF);
        if(!VF2.empty())
            tipl::downsampling(VF2);
        VFvs *= 2.0f;
        ratio /= 2.0f;
        std::cout << "ratio larger than 2.5, register using subject resolution of " << VFvs[0] << " mm resolution" << std::endl;
    }
}

void reconstruction_window::on_qsdr_manual_clicked()
{
    tipl::image<3> VG,VG2,dummy,VF(handle->dwi);
    tipl::vector<3> VGvs,VFvs(handle->voxel.vs);
    {
        gz_nifti read,read2;
        if(!read.load_from_file(fa_template_list[handle->voxel.template_id]))
        {
            QMessageBox::critical(this,"Error",QString("Cannot load template:"));
            return;
        }
        read.toLPS(VG);
        read.get_voxel_size(VGvs);
        if(read2.load_from_file(iso_template_list[handle->voxel.template_id]))
        {
            read2.toLPS(VG2);
            VG += VG2;
        }
    }

    match_template_resolution(VG,dummy,VGvs,VF,dummy,VFvs);
    std::shared_ptr<manual_alignment> manual(new manual_alignment(this,
                                                                VF,VFvs,VG,VGvs,
                                                                tipl::reg::affine,
                                                                tipl::reg::cost_type::mutual_info));
    manual->on_rerun_clicked();
    if(manual->exec() != QDialog::Accepted)
        return;
    handle->voxel.qsdr_trans = manual->get_iT();
}



void reconstruction_window::on_actionRun_FSL_Topup_triggered()
{
    QString other_src;
    if(!std::filesystem::exists(handle->file_name+".corrected.nii.gz"))
    {
        QMessageBox::information(this,"DSI Studio","Please specify another NIFTI or SRC.GZ file with reversed phase encoding data");
        other_src = QFileDialog::getOpenFileName(
                this,"Open SRC file",absolute_path,
                "Images (*src.gz *.nii *nii.gz);;DICOM image (*.dcm);;All files (*)" );
        if(other_src.isEmpty())
            return;
    }
    progress prog_("topup/eddy",true);
    if(command("[Step T2][Corrections][TOPUP EDDY]",other_src.toStdString()))
        QMessageBox::information(this,"DSI Studio","Correction result loaded");
}


void reconstruction_window::on_actionEDDY_triggered()
{
    progress prog_("eddy",true);
    if(command("[Step T2][Corrections][EDDY]"))
        QMessageBox::information(this,"DSI Studio","Correction result loaded");
}

void reconstruction_window::on_actionSmooth_Signals_triggered()
{
    command("[Step T2][Edit][Smooth Signals]");
}

