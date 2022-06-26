#include <regex>
#include <QFileDialog>
#include <QInputDialog>
#include <QContextMenuEvent>
#include <QMessageBox>
#include <QClipboard>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QHeaderView>
#include "regiontablewidget.h"
#include "tracking/tracking_window.h"
#include "qcolorcombobox.h"
#include "ui_tracking_window.h"
#include "mapping/atlas.hpp"
#include "opengl/glwidget.h"
#include "libs/tracking/fib_data.hpp"
#include "libs/tracking/tracking_thread.hpp"


void split(std::vector<std::string>& list,const std::string& input, const std::string& regex) {
    // passing -1 as the submatch index parameter performs splitting
    std::regex re(regex);
    std::sregex_token_iterator
        first{input.begin(), input.end(), re, -1},
        last;
    list = {first, last};
}

QWidget *ImageDelegate::createEditor(QWidget *parent,
                                     const QStyleOptionViewItem &option,
                                     const QModelIndex &index) const
{
    if (index.column() == 1)
    {
        QComboBox *comboBox = new QComboBox(parent);
        comboBox->addItem("ROI");
        comboBox->addItem("ROA");
        comboBox->addItem("End");
        comboBox->addItem("Seed");
        comboBox->addItem("Terminative");
        comboBox->addItem("NotEnd");
        comboBox->addItem("...");
        connect(comboBox, SIGNAL(activated(int)), this, SLOT(emitCommitData()));
        return comboBox;
    }
    else if (index.column() == 2)
    {
        QColorToolButton* sd = new QColorToolButton(parent);
        connect(sd, SIGNAL(clicked()), this, SLOT(emitCommitData()));
        return sd;
    }
    else
        return QItemDelegate::createEditor(parent,option,index);

}

void ImageDelegate::setEditorData(QWidget *editor,
                                  const QModelIndex &index) const
{

    if (index.column() == 1)
        dynamic_cast<QComboBox*>(editor)->setCurrentIndex(index.model()->data(index).toString().toInt());
    else
        if (index.column() == 2)
        {
            tipl::rgb color(uint32_t(index.data(Qt::UserRole).toInt()));
            dynamic_cast<QColorToolButton*>(editor)->setColor(
                QColor(color.r,color.g,color.b,color.a));
        }
        else
            return QItemDelegate::setEditorData(editor,index);
}

void ImageDelegate::setModelData(QWidget *editor, QAbstractItemModel *model,
                                 const QModelIndex &index) const
{
    if (index.column() == 1)
        model->setData(index,QString::number(dynamic_cast<QComboBox*>(editor)->currentIndex()));
    else
        if (index.column() == 2)
            model->setData(index,int((dynamic_cast<QColorToolButton*>(editor)->color().rgba())),Qt::UserRole);
        else
            QItemDelegate::setModelData(editor,model,index);
}

void ImageDelegate::emitCommitData()
{
    emit commitData(qobject_cast<QWidget *>(sender()));
}


RegionTableWidget::RegionTableWidget(tracking_window& cur_tracking_window_,QWidget *parent):
        QTableWidget(parent),cur_tracking_window(cur_tracking_window_)
{
    setColumnCount(4);
    setColumnWidth(0,140);
    setColumnWidth(1,60);
    setColumnWidth(2,40);
    setColumnWidth(3,200);
    horizontalHeader()->setDefaultAlignment(Qt::AlignLeft);

    QStringList header;
    header << "Name" << "Type" << "Color" << "Dimension × Resolution (mm)";
    setHorizontalHeaderLabels(header);
    setSelectionBehavior(QAbstractItemView::SelectRows);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setAlternatingRowColors(true);
    setStyleSheet("QTableView {selection-background-color: #AAAAFF; selection-color: #000000;}");

    setItemDelegate(new ImageDelegate(this));

    connect(this,SIGNAL(cellClicked(int,int)),this,SLOT(check_check_status(int,int)));
    connect(this,SIGNAL(itemChanged(QTableWidgetItem*)),this,SLOT(updateRegions(QTableWidgetItem*)));
    setEditTriggers(QAbstractItemView::DoubleClicked|QAbstractItemView::EditKeyPressed);
}


void RegionTableWidget::contextMenuEvent ( QContextMenuEvent * event )
{
    if (event->reason() == QContextMenuEvent::Mouse)
    {
        cur_tracking_window.ui->menuRegions->popup(event->globalPos());
    }
}

void RegionTableWidget::updateRegions(QTableWidgetItem* item)
{
    if (item->column() == 1)
        regions[uint32_t(item->row())]->regions_feature = uint8_t(item->text().toInt());
    else
        if (item->column() == 2)
        {
            regions[uint32_t(item->row())]->show_region.color = uint32_t(item->data(Qt::UserRole).toInt());
            emit need_update();
        }
}

QColor RegionTableWidget::currentRowColor(void)
{
    return uint32_t(regions[uint32_t(currentRow())]->show_region.color);
}
void RegionTableWidget::add_region_from_atlas(std::shared_ptr<atlas> at,unsigned int label)
{
    add_region(at->get_list()[label].c_str());
    std::vector<tipl::vector<3,short> > points;
    cur_tracking_window.handle->get_atlas_roi(at,label,regions.back()->dim,regions.back()->to_diffusion_space,points);
    if(points.empty())
        return;
    regions.back()->add_points(std::move(points));
}
void RegionTableWidget::add_all_regions_from_atlas(std::shared_ptr<atlas> at)
{
    std::vector<std::vector<tipl::vector<3,short> > > points;
    std::vector<std::string> labels;
    if(!cur_tracking_window.handle->get_atlas_all_roi(at,
            cur_tracking_window.current_slice->dim,
            cur_tracking_window.current_slice->T,points,labels))
    {
        QMessageBox::critical(this,"ERROR",cur_tracking_window.handle->error_msg.c_str());
        return;
    }
    for(unsigned int i = 0;i < labels.size();++i)
        add_region(labels[i].c_str());

    tipl::par_for(points.size(),[&](unsigned int i)
    {
        regions[regions.size()-points.size()+i]->add_points(std::move(points[i]));
    });
}
void RegionTableWidget::begin_update(void)
{
    cur_tracking_window.scene.no_show = true;
    cur_tracking_window.disconnect(cur_tracking_window.regionWidget,SIGNAL(need_update()),cur_tracking_window.glWidget,SLOT(update()));
    cur_tracking_window.disconnect(cur_tracking_window.regionWidget,SIGNAL(cellChanged(int,int)),cur_tracking_window.glWidget,SLOT(update()));
}

void RegionTableWidget::end_update(void)
{
    cur_tracking_window.scene.no_show = false;
    cur_tracking_window.connect(cur_tracking_window.regionWidget,SIGNAL(need_update()),cur_tracking_window.glWidget,SLOT(update()));
    cur_tracking_window.connect(cur_tracking_window.regionWidget,SIGNAL(cellChanged(int,int)),cur_tracking_window.glWidget,SLOT(update()));
}

void RegionTableWidget::add_row(int row,QString name)
{
    {
        uint32_t color = uint32_t(regions[row]->show_region.color);
        if(color == 0x00FFFFFF || !color)
        {
            tipl::rgb c;
            ++color_gen;
            c.from_hsl(((color_gen)*1.1-std::floor((color_gen)*1.1/6)*6)*3.14159265358979323846/3.0,0.85,0.7);
            regions[row]->show_region.color = c.color;
        }
    }
    auto handle = cur_tracking_window.handle;
    insertRow(row);
    QTableWidgetItem *item0 = new QTableWidgetItem(name);
    QTableWidgetItem *item1 = new QTableWidgetItem(QString::number(int(regions[row]->regions_feature)));
    QTableWidgetItem *item2 = new QTableWidgetItem();
    QTableWidgetItem *item3 = new QTableWidgetItem(QString("(%1,%2,%3)x(%4,%5,%6)")
                                                    .arg(regions[row]->dim[0])
                                                    .arg(regions[row]->dim[1])
                                                    .arg(regions[row]->dim[2])
                                                    .arg(regions[row]->vs[0])
                                                    .arg(regions[row]->vs[1])
                                                    .arg(regions[row]->vs[2]));

    item1->setData(Qt::ForegroundRole,QBrush(Qt::white));
    item2->setData(Qt::ForegroundRole,QBrush(Qt::white));
    item2->setData(Qt::UserRole,0xFF000000 | uint32_t(regions[row]->show_region.color));


    setItem(row, 0, item0);
    setItem(row, 1, item1);
    setItem(row, 2, item2);
    setItem(row, 3, item3);


    openPersistentEditor(item1);
    openPersistentEditor(item2);

    setRowHeight(row,22);
    setCurrentCell(row,0);

    if(cur_tracking_window.ui->target->count())
        cur_tracking_window.ui->target->setCurrentIndex(0);

    check_row(row,true);

}
void RegionTableWidget::add_region(QString name,unsigned char feature,unsigned int color)
{
    regions.push_back(std::make_shared<ROIRegion>(cur_tracking_window.handle));
    regions.back()->show_region.color = color;
    regions.back()->regions_feature = feature;
    regions.back()->dim = cur_tracking_window.current_slice->dim;
    regions.back()->vs = cur_tracking_window.current_slice->vs;
    regions.back()->is_diffusion_space = cur_tracking_window.current_slice->is_diffusion_space;
    regions.back()->to_diffusion_space = cur_tracking_window.current_slice->T;
    add_row(int(regions.size()-1),name);
}
void RegionTableWidget::check_check_status(int row, int col)
{
    if (col != 0)
        return;
    setCurrentCell(row,col);
    if (item(row,0)->checkState() == Qt::Checked)
    {
        if (item(row,0)->data(Qt::ForegroundRole) == QBrush(Qt::gray))
        {
            item(row,0)->setData(Qt::ForegroundRole,QBrush(Qt::black));
            emit need_update();
        }
    }
    else
    {
        if (item(row,0)->data(Qt::ForegroundRole) != QBrush(Qt::gray))
        {
            item(row,0)->setData(Qt::ForegroundRole,QBrush(Qt::gray));
            emit need_update();
        }
    }
}

bool RegionTableWidget::command(QString cmd,QString param,QString)
{
    if(cmd == "save_all_regions_to_dir")
    {
        progress prog_("save files...");
        for(int index = 0;progress::at(index,rowCount());++index)
            if (item(index,0)->checkState() == Qt::Checked) // either roi roa end or seed
            {
                std::string filename = param.toStdString();
                filename  += "/";
                filename  += item(index,0)->text().toStdString();
                filename  += output_format().toStdString();
                regions[size_t(index)]->SaveToFile(filename.c_str());
            }
        return true;
    }
    if(cmd == "detele_all_region")
    {
        delete_all_region();
        return true;
    }
    if(cmd == "load_region")
    {
        if(!load_multiple_roi_nii(param,false))
            return false;
        emit need_update();
        return true;
    }
    if(cmd == "load_mni_region")
    {
        if(!load_multiple_roi_nii(param,true))
            return false;
        emit need_update();
        return true;
    }
    if(cmd == "check_all_regions")
    {
        check_all();
        return true;
    }
    return false;
}
void RegionTableWidget::move_slice_to_current_region(void)
{
    if(currentRow() == -1)
        return;
    auto current_slice = cur_tracking_window.current_slice;
    auto current_region = regions[currentRow()];
    if(current_region->region.empty())
        return;
    tipl::vector<3,float> p(current_region->get_center());
    if(!current_slice->is_diffusion_space)
        p.to(current_slice->invT);
    cur_tracking_window.move_slice_to(p);
}


void RegionTableWidget::draw_region(std::shared_ptr<SliceModel> current_slice,
                                    unsigned char dim,int line_width,unsigned char draw_edges,
                                    const tipl::color_image& slice_image,float display_ratio,QImage& scaled_image)
{
    // during region removal, there will be a call with invalid currentRow
    auto checked_regions = get_checked_regions();
    if(checked_regions.empty() || currentRow() >= int(regions.size()) || currentRow() == -1)
        return;

    // draw region colors on the image
    tipl::color_image slice_image_with_region(slice_image.shape());  //original slices for adding regions pixels
    std::vector<std::vector<int> > region_pixels_x_edges(checked_regions.size()),
                                   region_pixels_y_edges(checked_regions.size());
    int w = slice_image.width();
    int h = slice_image.height();

    // draw regions and also derive where the edges are
    std::vector<std::vector<tipl::vector<2,int> > > edge_x(checked_regions.size()),
                                                    edge_y(checked_regions.size());
    {
        int slice_pos = current_slice->slice_pos[dim];
        std::vector<uint32_t> yw((size_t(h)));
        for(uint32_t y = 0;y < uint32_t(h);++y)
            yw[y] = y*uint32_t(w);

        for (uint32_t roi_index = 0;roi_index < checked_regions.size();++roi_index)
        {
            tipl::image<2,uint8_t> detect_edge(slice_image.shape());
            auto color = checked_regions[roi_index]->show_region.color;
            bool draw_roi = (draw_edges == 0 && color.a >= 128);

            if(current_slice->T != checked_regions[roi_index]->to_diffusion_space)
            {
                auto iT = tipl::from_space(checked_regions[roi_index]->to_diffusion_space).to(current_slice->T);
                tipl::transformation_matrix<float> m = iT;
                uint32_t x_range = 0;
                uint32_t y_range = 0;
                uint32_t z_range = 0;
                tipl::vector<3,float> p;
                tipl::slice2space(dim,1.0f,0.0f,0.0f,p[0],p[1],p[2]);
                p.rotate(m);
                x_range = uint32_t(std::ceil(p.length()));
                tipl::slice2space(dim,0.0f,1.0f,0.0f,p[0],p[1],p[2]);
                p.rotate(m);
                y_range = uint32_t(std::ceil(p.length()));
                tipl::slice2space(dim,0.0f,0.0f,1.0f,p[0],p[1],p[2]);
                p.rotate(m);
                z_range = uint32_t(std::ceil(p.length()));

                tipl::par_for(checked_regions[roi_index]->region.size(),[&](unsigned int index)
                {
                    tipl::vector<3,float> p(checked_regions[roi_index]->region[index]),p2;
                    p.to(iT);
                    tipl::space2slice(dim,p[0],p[1],p[2],p2[0],p2[1],p2[2]);
                    if (std::fabs(float(slice_pos)-p2[2]) >= z_range || p2[0] < 0.0f || p2[1] < 0.0f ||
                        int(p2[0]) >= w || int(p2[1]) >= h)
                        return;
                    uint32_t iX = uint32_t(std::floor(p2[0]));
                    uint32_t iY = uint32_t(std::floor(p2[1]));
                    uint32_t pos = yw[iY]+iX;
                    for(uint32_t dy = 0;dy < y_range;++dy,pos += uint32_t(w))
                        for(uint32_t dx = 0,pos2 = pos;dx < x_range;++dx,++pos2)
                        {
                            if(draw_roi)
                                slice_image_with_region[pos2] = color;
                            detect_edge[pos2] = 1;
                        }
                });
            }
            else
            {
                tipl::par_for(checked_regions[roi_index]->region.size(),[&](unsigned int index)
                {
                    int X, Y, Z;
                    auto p = checked_regions[roi_index]->region[index];
                    tipl::space2slice(dim,p[0],p[1],p[2],X,Y,Z);
                    if (slice_pos != Z || X < 0 || Y < 0 || X >= w || Y >= h)
                        return;
                    auto pos = uint32_t(yw[uint32_t(Y)]+uint32_t(X));
                    if(draw_roi)
                        slice_image_with_region[pos] = color;
                    detect_edge[pos] = 1;
                });
            }
            // detect edge
            auto& cur_edge_x = edge_x[roi_index];
            auto& cur_edge_y = edge_y[roi_index];
            for(int y = 1,cur_index = w;y < h-1;++y)
            for(int x = 0;x < w;++x,++cur_index)
            if(detect_edge[cur_index])
            {
                if(x == 0 || x+1 >= w)
                    continue;
                if(!detect_edge[cur_index-w])
                    cur_edge_x.push_back(tipl::vector<2,int>(int(x*display_ratio),int(y*display_ratio)));
                if(!detect_edge[cur_index+w])
                    cur_edge_x.push_back(tipl::vector<2,int>(int(x*display_ratio),int(y*display_ratio+display_ratio)));
                if(!detect_edge[cur_index-1])
                    cur_edge_y.push_back(tipl::vector<2,int>(int(x*display_ratio),int(y*display_ratio)));
                if(!detect_edge[cur_index+1])
                    cur_edge_y.push_back(tipl::vector<2,int>(int(x*display_ratio+display_ratio),int(y*display_ratio)));
            }
        }
    }
    // now apply image scaling to the slice image
    QImage qimage(reinterpret_cast<unsigned char*>(&*slice_image_with_region.begin()),
                  slice_image_with_region.width(),slice_image_with_region.height(),QImage::Format_RGB32);
    qimage.detach();
    scaled_image = qimage.scaled(int(slice_image.width()*display_ratio),
                                        int(slice_image.height()*display_ratio));


    // the following draw the edge of the regions
    int cur_roi_index = -1;
    if(currentRow() >= 0)
    for(unsigned int i = 0;i < checked_regions.size();++i)
        if(checked_regions[i] == regions[uint32_t(currentRow())])
        {
            cur_roi_index = int(i);
            break;
        }

    unsigned int foreground_color = ((scaled_image.pixel(0,0) & 0x000000FF) < 128 ? 0xFFFFFFFF:0xFF000000);
    int length = int(display_ratio);
    QPainter paint(&scaled_image);
    for (uint32_t roi_index = 0;roi_index < checked_regions.size();++roi_index)
    {
        unsigned int cur_color = foreground_color;
        if(draw_edges == 1 && (int(roi_index) != cur_roi_index))
            cur_color = checked_regions[roi_index]->show_region.color;
        paint.setBrush(Qt::NoBrush);
        QPen pen(QColor(cur_color),line_width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        paint.setPen(pen);
        for(const auto& pos : edge_x[roi_index])
            paint.drawLine(pos[0],pos[1],pos[0]+length,pos[1]);
        for(const auto& pos : edge_y[roi_index])
            paint.drawLine(pos[0],pos[1],pos[0],pos[1]+length);
    }
}

void RegionTableWidget::new_region(void)
{
    add_region("New Region");
}

void RegionTableWidget::copy_region(void)
{
    if(currentRow() < 0)
        return;
    unsigned int cur_row = uint32_t(currentRow());
    unsigned int color = regions[cur_row]->show_region.color.color;
    regions.insert(regions.begin() + cur_row + 1,std::make_shared<ROIRegion>(cur_tracking_window.handle));
    *regions[cur_row + 1] = *regions[cur_row];
    regions[cur_row + 1]->show_region.color.color = color;
    add_row(int(cur_row+1),item(currentRow(),0)->text());
}
void load_nii_label(const char* filename,std::map<int,std::string>& label_map)
{
    std::ifstream in(filename);
    if(in)
    {
        std::string line,txt;
        while(std::getline(in,line))
        {
            if(line.empty() || line[0] == '#')
                continue;
            std::istringstream read_line(line);
            int num = 0;
            read_line >> num >> txt;
            label_map[num] = txt;
        }
    }
}
void load_jason_label(const char* filename,std::map<int,std::string>& label_map)
{
    std::ifstream in(filename);
    if(!in)
        return;
    std::string line,label;
    while(std::getline(in,line))
    {
        std::replace(line.begin(),line.end(),'\"',' ');
        std::replace(line.begin(),line.end(),'\t',' ');
        std::replace(line.begin(),line.end(),',',' ');
        line.erase(std::remove(line.begin(),line.end(),' '),line.end());
        if(line.find("arealabel") != std::string::npos)
        {
            label = line.substr(line.find(":")+1,std::string::npos);
            continue;
        }
        if(line.find("labelIndex") != std::string::npos)
        {
            line = line.substr(line.find(":")+1,std::string::npos);
            std::istringstream in3(line);
            int num = 0;
            in3 >> num;
            label_map[num] = label;
        }
    }
}

void get_roi_label(QString file_name,std::map<int,std::string>& label_map,std::map<int,tipl::rgb>& label_color)
{
    label_map.clear();
    label_color.clear();
    QString base_name = QFileInfo(file_name).baseName();
    std::cout << "looking for region label file" << std::endl;

    QString label_file = QFileInfo(file_name).absolutePath()+"/"+base_name+".txt";
    if(QFileInfo(label_file).exists())
    {
        load_nii_label(label_file.toLocal8Bit().begin(),label_map);
        std::cout << "label file loaded:" << label_file.toStdString() << std::endl;
        return;
    }
    label_file = QFileInfo(file_name).absolutePath()+"/"+base_name+".json";
    if(QFileInfo(label_file).exists())
    {
        load_jason_label(label_file.toLocal8Bit().begin(),label_map);
        std::cout << "jason file loaded:" << label_file.toStdString() << std::endl;
        return;
    }
    if(base_name.contains("aparc") || base_name.contains("aseg")) // FreeSurfer
    {
        std::cout << "using freesurfer labels." << std::endl;
        QFile data(":/data/FreeSurferColorLUT.txt");
        if (data.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            QTextStream in(&data);
            while (!in.atEnd())
            {
                QString line = in.readLine();
                if(line.isEmpty() || line[0] == '#')
                    continue;
                std::istringstream in(line.toStdString());
                int value,r,b,g;
                std::string name;
                in >> value >> name >> r >> g >> b;
                label_map[value] = name;
                label_color[value] = tipl::rgb(uint8_t(r),uint8_t(g),uint8_t(b));
            }
            return;
        }
    }
    std::cout << "no label file found. Use default ROI numbering." << std::endl;
}
bool is_label_image(const tipl::image<3>& I);
bool load_nii(std::shared_ptr<fib_data> handle,
              const std::string& file_name,
              std::vector<std::pair<tipl::shape<3>,tipl::matrix<4,4> > >& transform_lookup,
              std::vector<std::shared_ptr<ROIRegion> >& regions,
              std::vector<std::string>& names,
              std::string& error_msg,
              bool is_mni_image)
{
    gz_nifti header;
    if (!header.load_from_file(file_name.c_str()))
    {
        error_msg = "cannot load the file. Please check access permision.";
        return false;
    }
    bool is_4d = header.dim(4) > 1;
    tipl::image<3,unsigned int> from;

    if(is_4d)
        from.resize(tipl::shape<3>(header.dim(1),header.dim(2),header.dim(3)));
    else
    {
        tipl::image<3> tmp;
        header.toLPS(tmp);
        if(header.is_integer() || is_label_image(tmp))
            from = tmp;
        else
        {
            from.resize(tmp.shape());
            for(size_t i = 0;i < from.size();++i)
                from[i] = (tmp[i] == 0.0f ? 0:1);
        }
    }

    tipl::vector<3> vs;
    tipl::matrix<4,4> T;
    header.get_image_transformation(T);
    header.get_voxel_size(vs);

    {
        std::cout << "DWI : " << handle->dim << " at " << handle->vs << std::endl;
        std::cout << "NIFTI : " << from.shape() << " at " << vs << std::endl;
    }
    std::vector<unsigned short> value_list;
    std::vector<unsigned short> value_map(std::numeric_limits<unsigned short>::max()+1);

    if(is_4d)
    {
        value_list.resize(header.dim(4));
        for(unsigned int index = 0;index <value_list.size();++index)
        {
            value_list[index] = uint16_t(index);
            value_map[uint16_t(index)] = 0;
        }
    }
    else
    {
        unsigned short max_value = 0;
        for (tipl::pixel_index<3>index(from.shape());index < from.size();++index)
        {
            if(from[index.index()] >= value_map.size())
                return false;
            value_map[from[index.index()]] = 1;
            max_value = std::max<unsigned short>(uint16_t(from[index.index()]),max_value);
        }
        for(unsigned short value = 1;value <= max_value;++value)
            if(value_map[value])
            {
                value_map[value] = uint16_t(value_list.size());
                value_list.push_back(value);
            }
    }

    bool multiple_roi = value_list.size() > 1;

    std::cout << (multiple_roi ? "nifti loaded as multiple ROI file":"nifti loaded as single ROI file") << std::endl;

    std::map<int,std::string> label_map;
    std::map<int,tipl::rgb> label_color;

    std::string des(header.get_descrip());
    if(multiple_roi)
        get_roi_label(file_name.c_str(),label_map,label_color);

    bool need_trans = false;
    tipl::matrix<4,4> to_diffusion_space = tipl::identity_matrix();

    if(from.shape() != handle->dim)
    {
        std::cout << "NIFTI file has a different dimension from DWI." << std::endl;

        if(handle->is_qsdr)
        {
            for(unsigned int index = 0;index < handle->view_item.size();++index)
                if(handle->view_item[index].native_geo == from.shape())
                {
                    if(handle->get_native_position().empty())
                    {
                        error_msg = "NIFTI is from the native space, and current FIB file is obsolete. Please reconstruct FIB file again to enable warpping.";
                        return false;
                    }
                    auto T = handle->view_item[index].native_trans;
                    tipl::image<3,unsigned int> new_from(handle->dim);
                    tipl::par_for(new_from.size(),[&](size_t i)
                    {
                        auto pos = handle->get_native_position()[i];
                        T(pos);
                        tipl::estimate<tipl::interpolation::nearest>(from,pos,new_from[i]);
                    });
                    std::cout << "applying QSDR warpping to the native space NIFTI file" << std::endl;
                    new_from.swap(from);
                    goto end;
                }
        }

        for(unsigned int index = 0;index < transform_lookup.size();++index)
            if(from.shape() == transform_lookup[index].first)
            {
                std::cout << "applying loaded t1wt2w transformation." << std::endl;
                to_diffusion_space = transform_lookup[index].second;
                need_trans = true;
                goto end;
            }

        if(handle->is_qsdr || handle->is_mni_image)
        {
            std::cout << "loaded NIFTI file used as MNI space image." << std::endl;
            to_diffusion_space = tipl::from_space(T).to(handle->trans_to_mni);
            need_trans = true;
            goto end;
        }

        if(header.is_mni() || is_mni_image)
        {
            std::cout << "warpping the NIFTI file from MNI space to the native space." << std::endl;
            if(!handle->mni2sub<tipl::interpolation::nearest>(from,T))
            {
                error_msg = handle->error_msg;
                return false;
            }
            goto end;
        }

        error_msg = "Cannot align ROI file with DWI.";
        if(has_gui)
            error_msg += "Please insert its reference T1W/T2W using [Slices][Insert T1WT2W] to guide the registration.";
        else
            error_msg += "use --t1t2 to load the reference T1W/T2W to guide the registration.";
        return false;
    }
    end:
    // single region ROI
    if(!multiple_roi)
    {
        names.push_back(QFileInfo(file_name.c_str()).baseName().toStdString());
        regions.push_back(std::make_shared<ROIRegion>(handle));
        if(need_trans)
        {
            regions.back()->dim = from.shape();
            regions.back()->vs = vs;
            regions.back()->is_diffusion_space = false;
            regions.back()->to_diffusion_space = to_diffusion_space;
        }
        tipl::image<3,unsigned char> mask(from);
        regions.back()->LoadFromBuffer(mask);

        unsigned int color = 0x00FFFFFF;
        unsigned int type = default_id;

        try{
            std::vector<std::string> info;
            split(info,header.get_descrip(),";");
            for(unsigned int index = 0;index < info.size();++index)
            {
                std::vector<std::string> name_value;
                split(name_value,info[index],"=");
                if(name_value.size() != 2)
                    continue;
                if(name_value[0] == "color")
                    std::istringstream(name_value[1]) >> color;
                if(name_value[0] == "roi")
                    std::istringstream(name_value[1]) >> type;
            }
        }catch(...){}
        regions.back()->show_region.color = color;
        regions.back()->regions_feature = uint8_t(type);
        return true;
    }

    std::vector<std::vector<tipl::vector<3,short> > > region_points(value_list.size());
    if(is_4d)
    {
        progress prog_("loading");
        for(size_t region_index = 0;progress::at(region_index,region_points.size());++region_index)
        {
            header.toLPS(from);
            for (tipl::pixel_index<3> index(from.shape());index < from.size();++index)
                if(from[index.index()])
                    region_points[region_index].push_back(index);
        }
    }
    else
    {
        for (tipl::pixel_index<3>index(from.shape());index < from.size();++index)
            if(from[index.index()])
                region_points[value_map[from[index.index()]]].push_back(index);
    }

    for(uint32_t i = 0;i < region_points.size();++i)
        {
            unsigned short value = value_list[i];
            QString name = (label_map.find(value) == label_map.end() ?
                 QFileInfo(file_name.c_str()).baseName() + "_" + QString::number(value):QString(label_map[value].c_str()));
            regions.push_back(std::make_shared<ROIRegion>(handle));
            names.push_back(name.toStdString());
            if(need_trans)
            {
                regions.back()->dim = from.shape();
                regions.back()->vs = vs;
                regions.back()->is_diffusion_space = false;
                regions.back()->to_diffusion_space = to_diffusion_space;
            }
            regions.back()->show_region.color = label_color.empty() ? 0x00FFFFFF : label_color[value].color;
            if(!region_points[i].empty())
                regions.back()->add_points(std::move(region_points[i]));
        }
    std::cout << "a total of " << regions.size() << " regions are loaded." << std::endl;
    if(regions.empty())
    {
        error_msg = "empty region file";
        return false;
    }
    return true;
}

bool RegionTableWidget::load_multiple_roi_nii(QString file_name,bool is_mni_image)
{
    QStringList files = file_name.split(",");
    std::vector<std::pair<tipl::shape<3>,tipl::matrix<4,4> > > transform_lookup;
    // searching for T1/T2 mappings
    for(unsigned int index = 0;index < cur_tracking_window.slices.size();++index)
    {
        auto slice = cur_tracking_window.slices[index];
        if(!slice->is_diffusion_space)
            transform_lookup.push_back(std::make_pair(slice->dim,slice->T));
    }
    std::vector<std::vector<std::shared_ptr<ROIRegion> > > loaded_regions(files.size());
    std::vector<std::vector<std::string> > names(files.size());

    {
        progress prog_("reading");
        size_t prog = 0;
        bool failed = false;
        tipl::par_for(files.size(),[&](unsigned int i)
        {
            if(progress::aborted() || failed)
                return;
            progress::at(prog++,files.size());
            if(QFileInfo(files[i]).suffix() == "gz" ||
                QFileInfo(files[i]).suffix() == "nii" ||
                QFileInfo(files[i]).suffix() == "hdr")
            {
                if(!load_nii(cur_tracking_window.handle,
                         files[i].toStdString(),
                         transform_lookup,
                         loaded_regions[i],
                         names[i],
                         error_msg,is_mni_image))
                {
                    failed = true;
                    return;
                }
            }
            else
            {
                std::shared_ptr<ROIRegion> region(new ROIRegion(cur_tracking_window.handle));
                if(!region->LoadFromFile(files[i].toStdString().c_str()))
                {
                    error_msg = "Cannot read file:";
                    error_msg += files[i].toStdString();
                    failed = true;
                    return;
                }
                loaded_regions[i].push_back(region);
                names[i].push_back(QFileInfo(files[i]).baseName().toStdString());
            }
        });

        if(progress::aborted())
            return true;
        if(failed)
            return false;
    }

    tipl::aggregate_results(std::move(loaded_regions),loaded_regions[0]);
    tipl::aggregate_results(std::move(names),names[0]);

    {
        progress prog_("loading ROIs");
        begin_update();
        for(uint32_t i = 0;progress::at(i,loaded_regions[0].size());++i)
            {
                regions.push_back(loaded_regions[0][i]);
                add_row(int(regions.size()-1),names[0][i].c_str());
                check_row(currentRow(),loaded_regions.size() == 1);
            }
        end_update();
    }
    return true;
}

void RegionTableWidget::load_region_color(void)
{
    QString filename = QFileDialog::getOpenFileName(
                this,"Load region color",QFileInfo(cur_tracking_window.windowTitle()).absolutePath()+"/region_color.txt",
                "Color files (*.txt);;All files (*)");
    if(filename.isEmpty())
        return;

    std::ifstream in(filename.toStdString().c_str());
    if (!in)
        return;
    std::vector<int> colors((std::istream_iterator<float>(in)),
                              (std::istream_iterator<float>()));
    if(colors.size() == regions.size()*4) // RGBA
    {
        for(size_t index = 0,pos = 0;index < regions.size() && pos+2 < colors.size();++index,pos+=4)
        {
            tipl::rgb c(std::min<int>(colors[pos],255),
                        std::min<int>(colors[pos+1],255),
                        std::min<int>(colors[pos+2],255),
                        std::min<int>(colors[pos+3],255));
            regions[index]->show_region.color = c;
            regions[index]->modified = true;
            item(int(index),2)->setData(Qt::UserRole,uint32_t(c));
        }
    }
    else
    //RGB
    {
        for(size_t index = 0,pos = 0;index < regions.size() && pos+2 < colors.size();++index,pos+=3)
        {
            tipl::rgb c(std::min<int>(colors[pos],255),
                        std::min<int>(colors[pos+1],255),
                        std::min<int>(colors[pos+2],255),255);
            regions[index]->show_region.color = c;
            regions[index]->modified = true;
            item(int(index),2)->setData(Qt::UserRole,uint32_t(c));
        }
    }
    emit need_update();
}
void RegionTableWidget::save_region_color(void)
{
    QString filename = QFileDialog::getSaveFileName(
                this,"Save region color",QFileInfo(cur_tracking_window.windowTitle()).absolutePath()+"/region_color.txt",
                "Color files (*.txt);;All files (*)");
    if(filename.isEmpty())
        return;

    std::ofstream out(filename.toStdString().c_str());
    if (!out)
        return;
    for(size_t index = 0;index < regions.size();++index)
    {
        tipl::rgb c(regions[index]->show_region.color);
        out << int(c[2]) << " " << int(c[1]) << " " << int(c[0]) << " " << int(c[3]) << std::endl;
    }
    QMessageBox::information(this,"DSI Studio","File saved");
}

void RegionTableWidget::load_region(void)
{
    QStringList filenames = QFileDialog::getOpenFileNames(
                                this,"Open region",QFileInfo(cur_tracking_window.windowTitle()).absolutePath(),"Region files (*.nii *.hdr *nii.gz *.mat);;Text files (*.txt);;All files (*)" );
    if (filenames.isEmpty())
        return;
    if(!command("load_region",filenames.join(",")))
        QMessageBox::critical(this,"ERROR",error_msg.c_str());
    emit need_update();
}

void RegionTableWidget::load_mni_region(void)
{
    QStringList filenames = QFileDialog::getOpenFileNames(
                                this,"Open region",QFileInfo(cur_tracking_window.windowTitle()).absolutePath(),"NIFTI files (*.nii *nii.gz);;All files (*)" );
    if (filenames.isEmpty() || !cur_tracking_window.map_to_mni())
        return;

    for (int index = 0;index < filenames.size();++index)
        if(!command("load_mni_region",filenames[index]))
        {
            QMessageBox::critical(this,"ERROR",error_msg.c_str());
            break;
        }
    emit need_update();
}

void convert_region(std::vector<tipl::vector<3,short> >& points,
                    const tipl::shape<3>& dim_from,
                    const tipl::matrix<4,4>& trans_from,
                    const tipl::shape<3>& dim_to,
                    const tipl::matrix<4,4>& trans_to);
void RegionTableWidget::merge_all(void)
{
    std::vector<size_t> merge_list;
    for(size_t index = 0;index < regions.size();++index)
        if(item(int(index),0)->checkState() == Qt::Checked)
            merge_list.push_back(index);
    if(merge_list.size() <= 1)
        return;

    tipl::image<3,unsigned char> mask(regions[merge_list[0]]->dim);
    progress prog_("merging regions",true);
    size_t prog = 0;
    tipl::par_for(merge_list.size(),[&](size_t index,unsigned int id)
    {
        if(progress::aborted())
            return;
        progress::at(prog++,merge_list.size());
        if(regions[merge_list[0]]->to_diffusion_space != regions[merge_list[index]]->to_diffusion_space)
                convert_region(regions[merge_list[index]]->region,
                               regions[merge_list[index]]->dim,
                               regions[merge_list[index]]->to_diffusion_space,
                               regions[merge_list[0]]->dim,
                               regions[merge_list[0]]->to_diffusion_space);
        for(auto& p: regions[merge_list[index]]->region)
        {
            if (mask.shape().is_valid(p))
                mask.at(p) = 1;
        }
    });
    if(progress::aborted())
        return;
    regions[merge_list[0]]->LoadFromBuffer(mask);
    begin_update();
    for(int index = merge_list.size()-1;index >= 1;--index)
    {
        regions.erase(regions.begin()+merge_list[index]);
        removeRow(merge_list[index]);
    }
    end_update();
    emit need_update();
}

void RegionTableWidget::check_row(size_t row,bool checked)
{
    if(checked)
    {
        item(row,0)->setCheckState(Qt::Checked);
        item(row,0)->setData(Qt::ForegroundRole,QBrush(Qt::black));
    }
    else
    {
        item(row,0)->setCheckState(Qt::Unchecked);
        item(row,0)->setData(Qt::ForegroundRole,QBrush(Qt::gray));
    }
}
void RegionTableWidget::check_all(void)
{
    cur_tracking_window.glWidget->no_update = true;
    cur_tracking_window.scene.no_show = true;
    for(int row = 0;row < rowCount();++row)
        check_row(row,true);
    cur_tracking_window.scene.no_show = false;
    cur_tracking_window.glWidget->no_update = false;
    emit need_update();
}

void RegionTableWidget::uncheck_all(void)
{
    cur_tracking_window.glWidget->no_update = true;
    cur_tracking_window.scene.no_show = true;
    for(int row = 0;row < rowCount();++row)
        check_row(row,false);
    cur_tracking_window.scene.no_show = false;
    cur_tracking_window.glWidget->no_update = false;
    emit need_update();
}

void RegionTableWidget::move_up(void)
{
    if(currentRow())
    {
        regions[uint32_t(currentRow())].swap(regions[uint32_t(currentRow())-1]);
        begin_update();
        for(int i = 0;i < 3;++i)
        {
            QTableWidgetItem* item0 = takeItem(currentRow()-1,i);
            QTableWidgetItem* item1 = takeItem(currentRow(),i);
            setItem(currentRow()-1,i,item1);
            setItem(currentRow(),i,item0);
        }
        end_update();
        setCurrentCell(currentRow()-1,0);
    }

}

void RegionTableWidget::move_down(void)
{
    if(currentRow()+1 < int(regions.size()))
    {
        regions[uint32_t(currentRow())].swap(regions[uint32_t(currentRow())+1]);
        begin_update();
        for(int i = 0;i < 3;++i)
        {
            QTableWidgetItem* item0 = takeItem(currentRow()+1,i);
            QTableWidgetItem* item1 = takeItem(currentRow(),i);
            setItem(currentRow()+1,i,item1);
            setItem(currentRow(),i,item0);
        }
        end_update();
        setCurrentCell(currentRow()+1,0);
    }
}

void RegionTableWidget::save_region(void)
{
    if (regions.empty() || currentRow() >= regions.size())
        return;
    QString filename = QFileDialog::getSaveFileName(
                           this,
                           "Save region",item(currentRow(),0)->text() + output_format(),"NIFTI file(*nii.gz *.nii);;Text file(*.txt);;MAT file (*.mat);;All files(*)" );
    if (filename.isEmpty())
        return;
    if(QFileInfo(filename.toLower()).completeSuffix() != "mat" && QFileInfo(filename.toLower()).completeSuffix() != "txt")
        filename = QFileInfo(filename).absolutePath() + "/" + QFileInfo(filename).baseName() + ".nii.gz";
    regions[currentRow()]->SaveToFile(filename.toLocal8Bit().begin());
    item(currentRow(),0)->setText(QFileInfo(filename).baseName());
}
QString RegionTableWidget::output_format(void)
{
    switch(cur_tracking_window["roi_format"].toInt())
    {
    case 0:
        return ".nii.gz";
    case 1:
        return ".mat";
    case 2:
        return ".txt";
    }
    return "";
}

void RegionTableWidget::save_all_regions_to_dir(void)
{
    if (regions.empty())
        return;
    QString dir = QFileDialog::getExistingDirectory(this,"Open directory","");
    if(dir.isEmpty())
        return;
    command("save_all_regions_to_dir",dir);
}

void RegionTableWidget::save_checked_region_label_file(QString filename,int first_index)
{
    QString base_name = QFileInfo(filename).completeBaseName();
    if(QFileInfo(base_name).suffix().toLower() == "nii")
        base_name = QFileInfo(base_name).completeBaseName();
    QString label_file = QFileInfo(filename).absolutePath()+"/"+base_name+".txt";
    std::ofstream out(label_file.toStdString().c_str());
    for (unsigned int roi_index = 0;roi_index < regions.size();++roi_index)
    {
        if (item(int(roi_index),0)->checkState() != Qt::Checked)
            continue;
        out << first_index << " " << item(int(roi_index),0)->text().toStdString() << std::endl;
        ++first_index;
    }
}

void RegionTableWidget::save_all_regions_to_4dnifti(void)
{
    auto checked_regions = get_checked_regions();
    if (checked_regions.empty())
        return;
    QString filename = QFileDialog::getSaveFileName(
                           this,"Save region",item(currentRow(),0)->text() + output_format(),
                           "Region file(*nii.gz *.nii);;All file types (*)" );
    if (filename.isEmpty())
        return;

    tipl::shape<3> dim = checked_regions[0]->dim;
    tipl::image<4,unsigned char> multiple_I(tipl::shape<4>(dim[0],dim[1],dim[2],uint32_t(checked_regions.size())));
    progress prog_("aggregating regions");
    size_t prog = 0;
    tipl::par_for (checked_regions.size(),[&](unsigned int region_index)
    {
        if(progress::aborted())
            return;
        progress::at(prog++,checked_regions.size());
        size_t offset = region_index*dim.size();
        auto points = checked_regions[region_index]->region;
        convert_region(points,
                       checked_regions[region_index]->dim,
                       checked_regions[region_index]->to_diffusion_space,
                       checked_regions[0]->dim,
                       checked_regions[0]->to_diffusion_space);
        for (auto& p : points)
        {
            if (dim.is_valid(p))
                multiple_I[offset+tipl::pixel_index<3>(p[0],p[1],p[2],dim).index()] = 1;
        }
    });

    if(progress::aborted())
        return;

    if(gz_nifti::save_to_file(filename.toStdString().c_str(),multiple_I,
                              checked_regions[0]->vs,
                              checked_regions[0]->trans_to_mni,
                              cur_tracking_window.handle->is_qsdr))
    {
        save_checked_region_label_file(filename,0);  // 4d nifti index starts from 0
        QMessageBox::information(this,"DSI Studio","saved");
    }
    else
        QMessageBox::critical(this,"ERROR","cannot write to file");
}
void RegionTableWidget::save_all_regions(void)
{
    auto checked_regions = get_checked_regions();
    if (checked_regions.empty())
        return;
    QString filename = QFileDialog::getSaveFileName(
                           this,"Save region",item(currentRow(),0)->text() + output_format(),
                           "Region file(*nii.gz *.nii *.mat);;Text file (*.txt);;All file types (*)" );
    if (filename.isEmpty())
        return;
    tipl::shape<3> dim = checked_regions[0]->dim;
    tipl::image<3,unsigned short> mask(dim);
    tipl::par_for (checked_regions.size(),[&](unsigned int region_index)
    {
        auto region_id = uint16_t(region_index+1);
        auto points = checked_regions[region_index]->region;
        convert_region(points,
                       checked_regions[region_index]->dim,
                       checked_regions[region_index]->to_diffusion_space,
                       checked_regions[0]->dim,
                       checked_regions[0]->to_diffusion_space);
        for (auto& p : points)
            if (dim.is_valid(p))
            {
                auto pos = tipl::pixel_index<3>(p[0],p[1],p[2],dim).index();
                if(mask[pos] < region_id)
                    mask[pos] = region_id;
            }
    });

    bool result;
    if(checked_regions.size() <= 255)
    {
        tipl::image<3,uint8_t> i8mask(mask);
        result = gz_nifti::save_to_file(filename.toStdString().c_str(),i8mask,
                           cur_tracking_window.current_slice->vs,
                           cur_tracking_window.handle->trans_to_mni,
                           cur_tracking_window.handle->is_qsdr || cur_tracking_window.handle->is_mni_image);
    }
    else
    {
        result = gz_nifti::save_to_file(filename.toStdString().c_str(),mask,
                           cur_tracking_window.current_slice->vs,
                           cur_tracking_window.handle->trans_to_mni,
                           cur_tracking_window.handle->is_qsdr || cur_tracking_window.handle->is_mni_image);
    }
    if(result)
    {
        save_checked_region_label_file(filename,1); // 3d nifti index starts from 1
        QMessageBox::information(this,"DSI Studio","saved");
    }
    else
        QMessageBox::critical(this,"ERROR","cannot write to file");
}

void RegionTableWidget::save_region_info(void)
{
    if (regions.empty() || currentRow() >= regions.size())
        return;

    if(!regions[currentRow()]->is_diffusion_space)
    {
        QMessageBox::critical(this,"ERROR","Voxels not in the DWI space");
        return;
    }
    QString filename = QFileDialog::getSaveFileName(
                           this,"Save voxel information",item(currentRow(),0)->text() + "_info.txt",
                           "Text files (*.txt)" );
    if (filename.isEmpty())
        return;

    std::ofstream out(filename.toLocal8Bit().begin());
    out << "x\ty\tz";
    for(unsigned int index = 0;index < cur_tracking_window.handle->dir.num_fiber;++index)
            out << "\tdx" << index << "\tdy" << index << "\tdz" << index;

    for(unsigned int index = 0;index < cur_tracking_window.handle->view_item.size();++index)
        if(cur_tracking_window.handle->view_item[index].name != "color" &&
           cur_tracking_window.handle->view_item[index].image_ready)
            out << "\t" << cur_tracking_window.handle->view_item[index].name;

    out << std::endl;
    auto points = regions[currentRow()]->region;
    convert_region(points,regions[currentRow()]->dim,
                          regions[currentRow()]->to_diffusion_space,
                          cur_tracking_window.handle->dim,
                          tipl::matrix<4,4>(tipl::identity_matrix()));
    for(auto& point : points)
    {
        std::vector<float> data;
        cur_tracking_window.handle->get_voxel_info2(point[0],point[1],point[2],data);
        cur_tracking_window.handle->get_voxel_information(point[0],point[1],point[2],data);
        std::copy(point.begin(),point.end(),std::ostream_iterator<float>(out,"\t"));
        std::copy(data.begin(),data.end(),std::ostream_iterator<float>(out,"\t"));
        out << std::endl;
    }
}

void RegionTableWidget::delete_region(void)
{
    if (currentRow() >= regions.size())
        return;
    regions.erase(regions.begin()+currentRow());
    removeRow(currentRow());
    emit need_update();
}

void RegionTableWidget::delete_all_region(void)
{
    setRowCount(0);
    regions.clear();
    emit need_update();
}


void get_regions_statistics(std::shared_ptr<fib_data> handle,const std::vector<std::shared_ptr<ROIRegion> >& regions,
                            const std::vector<std::string>& region_name,
                            std::string& result)
{
    std::vector<std::string> titles;
    std::vector<std::vector<float> > data(regions.size());
    tipl::par_for(regions.size(),[&](unsigned int index){
        std::vector<std::string> dummy;
        regions[index]->get_quantitative_data(handle,(index == 0) ? titles : dummy,data[index]);
    });
    std::ostringstream out;
    out << "Name\t";
    for(unsigned int index = 0;index < regions.size();++index)
        out << region_name[index] << "\t";
    out << std::endl;
    for(unsigned int i = 0;i < titles.size();++i)
    {
        out << titles[i] << "\t";
        for(unsigned int j = 0;j < regions.size();++j)
        {
            if(i < data[j].size())
                out << data[j][i];
            out << "\t";
        }
        out << std::endl;
    }
    result = out.str();
}

void RegionTableWidget::show_statistics(void)
{
    if(currentRow() >= regions.size())
        return;
    std::string result;
    {
        std::vector<std::shared_ptr<ROIRegion> > active_regions;
        std::vector<std::string> region_name;
        for(unsigned int index = 0;index < regions.size();++index)
            if(item(index,0)->checkState() == Qt::Checked)
            {
                active_regions.push_back(regions[index]);
                region_name.push_back(item(index,0)->text().toStdString());
            }
        get_regions_statistics(cur_tracking_window.handle,active_regions,region_name,result);
    }
    QMessageBox msgBox;
    msgBox.setText("Region Statistics");
    msgBox.setDetailedText(result.c_str());
    msgBox.setStandardButtons(QMessageBox::Ok|QMessageBox::Save);
    msgBox.setDefaultButton(QMessageBox::Ok);
    QPushButton *copyButton = msgBox.addButton("Copy To Clipboard", QMessageBox::ActionRole);


    if(msgBox.exec() == QMessageBox::Save)
    {
        QString filename;
        filename = QFileDialog::getSaveFileName(
                    this,"Save satistics as",item(currentRow(),0)->text() + "_stat.txt",
                    "Text files (*.txt);;All files|(*)");
        if(filename.isEmpty())
            return;
        std::ofstream out(filename.toLocal8Bit().begin());
        out << result.c_str();
    }
    if (msgBox.clickedButton() == copyButton)
        QApplication::clipboard()->setText(result.c_str());
}

void RegionTableWidget::whole_brain(void)
{
    auto cur_slice = cur_tracking_window.current_slice;
    float threshold = cur_tracking_window.get_fa_threshold();
    tipl::image<3,unsigned char> mask(cur_slice->dim);
    auto fa_map = tipl::make_image(cur_tracking_window.handle->dir.fa[0],cur_tracking_window.handle->dim);

    if(cur_slice->is_diffusion_space)
        tipl::par_for(mask.size(),[&](size_t index)
        {
            if(fa_map[index] > threshold)
                mask[index] = 1;
        });
    else
        tipl::par_for(tipl::begin_index(mask.shape()),
                      tipl::end_index(mask.shape()),
                      [&](const tipl::pixel_index<3>& index)
        {
            tipl::vector<3> pos(index);
            pos.to(cur_slice->T);
            if(tipl::estimate(fa_map,pos) > threshold)
                mask[index.index()] = 1;
        });
    add_region("whole brain",seed_id);
    regions.back()->LoadFromBuffer(mask);
    emit need_update();
}

void RegionTableWidget::setROIs(ThreadData* data)
{
    int roi_count = 0;
    for (unsigned int index = 0;index < regions.size();++index)
        if (!regions[index]->region.empty() && item(int(index),0)->checkState() == Qt::Checked
                && regions[index]->regions_feature == roi_id)
            ++roi_count;
    for (unsigned int index = 0;index < regions.size();++index)
        if (!regions[index]->region.empty() && item(int(index),0)->checkState() == Qt::Checked
                && !(regions[index]->regions_feature == roi_id && roi_count > 5) &&
                regions[index]->regions_feature != default_id)
            data->roi_mgr->setRegions(regions[index]->region,
                                      regions[index]->dim,
                                      regions[index]->to_diffusion_space,
                                      regions[index]->regions_feature,item(int(index),0)->text().toLocal8Bit().begin());
    // auto track
    if(cur_tracking_window.ui->target->currentIndex() > 0 &&
       cur_tracking_window.handle->track_atlas.get())
        data->roi_mgr->setAtlas(uint32_t(cur_tracking_window.ui->target->currentIndex()-1),
                                cur_tracking_window["autotrack_tolerance"].toFloat());

    if(data->roi_mgr->seeds.empty())
        data->roi_mgr->setWholeBrainSeed(cur_tracking_window.get_fa_threshold());
}

QString RegionTableWidget::getROIname(void)
{
    for (size_t index = 0;index < regions.size();++index)
        if (!regions[index]->region.empty() && item(int(index),0)->checkState() == Qt::Checked &&
             regions[index]->regions_feature == roi_id)
                return item(int(index),0)->text();
    for (size_t index = 0;index < regions.size();++index)
        if (!regions[index]->region.empty() && item(int(index),0)->checkState() == Qt::Checked &&
             regions[index]->regions_feature == seed_id)
                return item(int(index),0)->text();
    return "whole_brain";
}
void RegionTableWidget::undo(void)
{
    if(currentRow() < 0)
        return;
    regions[size_t(currentRow())]->undo();
    emit need_update();
}
void RegionTableWidget::redo(void)
{
    if(currentRow() < 0)
        return;
    regions[size_t(currentRow())]->redo();
    emit need_update();
}

void RegionTableWidget::do_action(QString action)
{
    if(regions.empty() || currentRow() < 0)
        return;
    size_t roi_index = currentRow();

    {
        ROIRegion& cur_region = *regions[size_t(roi_index)];
        if(cur_tracking_window.ui->actionModify_All->isChecked())
        {
            auto checked_regions = get_checked_regions();
            tipl::par_for(checked_regions.size(),[&](unsigned int i){checked_regions[i]->perform(action.toStdString());});
        }
        else
            cur_region.perform(action.toStdString());


        if(action == "A-B" || action == "B-A" || action == "A*B" || action == "A<<B")
        {
            auto checked_regions = get_checked_regions();
            if(checked_regions.size() < 2)
                return;
            for(auto r : checked_regions)
                if(!checked_regions[0]->is_same_space(*r.get()))
                {
                    QMessageBox::critical(this,"ERROR","Please resample regions to the same space");
                    return;
                }
            tipl::image<3,unsigned char> A;
            tipl::image<3,uint16_t> A_labels;
            checked_regions[0]->SaveToBuffer(A);
            if(action == "A<<B")
                A_labels.resize(A.shape());

            tipl::par_for(checked_regions.size(),[&](size_t r)
            {
                if(r == 0)
                    return;
                tipl::image<3,unsigned char> B;
                checked_regions[r]->SaveToBuffer(B);
                if(action == "A-B")
                {
                    for(size_t i = 0;i < A.size();++i)
                        if(B[i])
                            A[i] = 0;
                }
                if(action == "B-A")
                {
                    for(size_t i = 0;i < B.size();++i)
                        if(A[i])
                            B[i] = 0;
                    checked_regions[r]->LoadFromBuffer(B);
                }
                if(action == "A*B")
                {
                    for(size_t i = 0;i < B.size();++i)
                        B[i] = (A[i] & B[i]);
                    checked_regions[r]->LoadFromBuffer(B);
                }
                if(action == "A<<B")
                {
                    for(size_t i = 0;i < A.size();++i)
                        if(A[i] && B[i] && A_labels[i] < r)
                            A_labels[i] = uint16_t(r);
                }
            });
            if(action == "A-B")
                checked_regions[0]->LoadFromBuffer(A);

            if(action == "A<<B")
            {
                tipl::par_for(tipl::begin_index(A.shape()),
                              tipl::end_index(A.shape()),[&](const tipl::pixel_index<3>& index)
                {
                    if(!A[index.index()] || A_labels[index.index()])
                        return;
                    float min_dis = std::numeric_limits<float>::max();
                    size_t min_r = 1;
                    tipl::vector<3> pos(index);
                    for(size_t r = 1;r < checked_regions.size();++r)
                    {
                        for(auto& pos2 : checked_regions[r]->region)
                        {
                            {
                                if(std::abs(pos2[0]-pos[0]) > min_dis ||
                                   std::abs(pos2[1]-pos[1]) > min_dis ||
                                   std::abs(pos2[2]-pos[2]) > min_dis)
                                       continue;
                            }
                            pos2 -= pos;
                            float L = float(pos2.length());
                            if(L < min_dis)
                            {
                                min_dis = L;
                                min_r = r;
                            }
                        }
                    }
                    A_labels[index.index()] = min_r;
                });

                tipl::par_for(checked_regions.size(),[&](size_t r)
                {
                    if(r == 0)
                        return;
                    tipl::image<3,unsigned char> B(A_labels.shape());
                    for(size_t i = 0;i < A_labels.size();++i)
                        if(A_labels[i] == r)
                            B[i] = 1;
                    checked_regions[r]->LoadFromBuffer(B);
                });
            }
        }
        if(action == "dilation_by_voxel")
        {
            bool ok;
            int threshold = float(QInputDialog::getInt(this,"DSI Studio","Voxel distance",10,1,100,1,&ok));
            if(!ok)
                return;
            tipl::image<3,unsigned char> mask;
            cur_region.SaveToBuffer(mask);
            tipl::morphology::dilation2_mt(mask,threshold);
            cur_region.LoadFromBuffer(mask);
        }
        if(action == "threshold" || action == "threshold_current")
        {
            tipl::const_pointer_image<3,float> I = cur_tracking_window.current_slice->get_source();
            if(I.empty())
                return;
            double m = tipl::max_value(I);
            bool ok;
            bool flip = false;
            float threshold = float(QInputDialog::getDouble(this,
                "DSI Studio","Threshold (assign negative value to get low pass):",
                double(tipl::segmentation::otsu_threshold(I)),-m,m,4, &ok));
            if(!ok)
                return;
            if(threshold < 0)
            {
                flip = true;
                threshold = -threshold;
            }

            tipl::image<3,unsigned char> mask(I.shape());

            if(action == "threshold_current")
            {
                bool need_trans = false;
                tipl::matrix<4,4> trans = tipl::identity_matrix();
                if(cur_tracking_window.current_slice->dim != cur_region.dim ||
                   cur_tracking_window.current_slice->T != cur_region.to_diffusion_space)
                {
                    need_trans = true;
                    trans = cur_tracking_window.current_slice->invT*cur_region.to_diffusion_space;
                }

                cur_region.SaveToBuffer(mask);

                tipl::par_for(tipl::begin_index(mask.shape()),tipl::end_index(mask.shape()),
                              [&](const tipl::pixel_index<3>& pos)
                {
                    if(!mask[pos.index()])
                        return;
                    float value = 0.0f;
                    size_t i = pos.index();
                    if(need_trans)
                    {
                        tipl::vector<3> p(pos);
                        p.to(trans);
                        if(!tipl::estimate(I,p,value))
                            return;
                    }
                    else
                        value = I[i];
                    mask[i]  = ((value > threshold) ^ flip) ? 1:0;
                });
            }
            else
            {
                tipl::par_for(mask.size(),[&](size_t i)
                {
                    mask[i]  = ((I[i] > threshold) ^ flip) ? 1:0;
                });
            }
            cur_region.LoadFromBuffer(mask);

        }
        if(action == "separate")
        {
            tipl::image<3,unsigned char>mask;
            cur_region.SaveToBuffer(mask);
            QString name = item(roi_index,0)->text();
            tipl::image<3,unsigned int> labels;
            std::vector<std::vector<unsigned int> > r;
            tipl::morphology::connected_component_labeling(mask,labels,r);
            begin_update();
            for(unsigned int j = 0,total_count = 0;j < r.size() && total_count < 256;++j)
                if(!r[j].empty())
                {
                    mask = 0;
                    for(unsigned int i = 0;i < r[j].size();++i)
                        mask[r[j][i]] = 1;
                    regions.push_back(std::make_shared<ROIRegion>(cur_tracking_window.handle));
                    regions.back()->dim = cur_region.dim;
                    regions.back()->vs = cur_region.vs;
                    regions.back()->is_diffusion_space = cur_region.is_diffusion_space;
                    regions.back()->to_diffusion_space = cur_region.to_diffusion_space;
                    regions.back()->is_mni = cur_region.is_mni;
                    regions.back()->LoadFromBuffer(mask);
                    add_row(int(regions.size()-1),(name + "_"+QString::number(total_count+1)));
                    ++total_count;
                }
            end_update();
        }
        if(action.contains("sort_"))
        {
            // reverse when repeated
            bool negate = false;
            {
                static QString last_action;
                if(action == last_action)
                {
                    last_action  = "";
                    negate = true;
                }
            else
                last_action = action;
            }
            std::vector<unsigned int> arg;
            if(action == "sort_name")
            {
                arg = tipl::arg_sort(regions.size(),[&]
                (int lhs,int rhs)
                {
                    auto lstr = item(lhs,0)->text();
                    auto rstr = item(rhs,0)->text();
                    return negate ^ (lstr.length() == rstr.length() ? lstr < rstr : lstr.length() < rstr.length());
                });
            }
            else
            {
                std::vector<float> data(regions.size());
                tipl::par_for(regions.size(),[&](unsigned int index){
                    if(action == "sort_x")
                        data[index] = regions[index]->get_pos()[0];
                    if(action == "sort_y")
                        data[index] = regions[index]->get_pos()[1];
                    if(action == "sort_z")
                        data[index] = regions[index]->get_pos()[2];
                    if(action == "sort_size")
                        data[index] = regions[index]->get_volume();
                });

                arg = tipl::arg_sort(data,[negate](float lhs,float rhs){return negate ^ (lhs < rhs);});
            }

            std::vector<std::shared_ptr<ROIRegion> > new_region(arg.size());
            std::vector<int> new_region_checked(arg.size());
            std::vector<std::string> new_region_names(arg.size());
            for(size_t i = 0;i < arg.size();++i)
            {
                new_region[i] = regions[arg[i]];
                new_region_checked[i] = item(int(arg[i]),0)->checkState() == Qt::Checked ? 1:0;
                new_region_names[i] = item(int(arg[i]),0)->text().toStdString();
            }
            regions.swap(new_region);
            for(int i = 0;i < int(arg.size());++i)
            {
                check_row(i,new_region_checked[uint32_t(i)]);
                item(i,0)->setText(new_region_names[uint32_t(i)].c_str());
                closePersistentEditor(item(i,1));
                closePersistentEditor(item(i,2));
                item(i,1)->setData(Qt::DisplayRole,regions[uint32_t(i)]->regions_feature);
                item(i,2)->setData(Qt::UserRole,regions[uint32_t(i)]->show_region.color.color);
                openPersistentEditor(item(i,1));
                openPersistentEditor(item(i,2));
            }
        }
    }
    emit need_update();
}
