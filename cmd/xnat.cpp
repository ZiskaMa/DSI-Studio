
#include <xnat_dialog.h>


int xnat(program_option& po)
{
    std::string output = po.get("output");
    if(QFileInfo(output.c_str()).isDir() && output.back() != '\\' && output.back() != '/')
        output += '/';

    if(!po.has("id"))
    {
        if(output.empty() || QFileInfo(output.c_str()).isDir())
            output = "data.txt";
        if(!QString(output.c_str()).endsWith(".txt"))
            output += ".txt";
        show_progress() << "writing output to " << output << std::endl;
        xnat_connection.get_experiments_info(po.get("source","https://central.xnat.org/"),po.get("auth"));
    }
    else
    {
        if(output.empty())
            output = QDir::current().path().toStdString();
        if(!QFileInfo(output.c_str()).isDir())
        {
            show_progress() << "ERROR: please specify output directory using --output" << std::endl;
            return 1;
        }
        show_progress() << "writing output to " << output << std::endl;
        xnat_connection.get_scans_data(po.get("source","https://central.xnat.org/"),po.get("auth"),po.get("id"),output);
    }

    while(xnat_connection.is_running())
        QApplication::processEvents();

    if (xnat_connection.has_error())
    {
        show_progress() << "ERROR: " << xnat_connection.error_msg << std::endl;
        return 1;
    }

    if(!po.has("id"))
    {
        show_progress() << "write experiment info to " << output << std::endl;
        std::ofstream(output) << xnat_connection.result;
    }
    else
    {
        show_progress() << "data saved to " << output << std::endl;
    }
    return 0;
}
