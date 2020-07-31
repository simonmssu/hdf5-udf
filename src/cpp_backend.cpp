/*
 * HDF5-UDF: User-Defined Functions for HDF5
 *
 * File: main.cpp
 *
 * C++ code parser and shared library generation/execution.
 */
#include <stdio.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <fstream>
#include <sstream>
#include <string>
#include <algorithm>
#include "cpp_backend.h"
#include "dataset.h"

/* This backend's name */
std::string CppBackend::name()
{
    return "C++";
}

/* Extension managed by this backend */
std::string CppBackend::extension()
{
    return ".cpp";
}

/* Helper: saves a data buffer to a temporary file on disk */
std::string CppBackend::writeToDisk(const char *data, size_t size, std::string extension)
{
    char *tmp = getenv("TMPDIR") ? : (char *) "/tmp";
    char path[PATH_MAX];
    std::ofstream tmpfile;
    snprintf(path, sizeof(path)-1, "%s/hdf5-udf-XXXXXX%s", tmp, extension.c_str());
    if (mkstemps(path, extension.size()) < 0){
        fprintf(stderr, "Error creating temporary file.\n");
        return std::string("");
    }
    tmpfile.open(path);
    tmpfile.write(data, size);
    tmpfile.flush();
    tmpfile.close();
    return std::string(path);
}

/* Compile C to a shared object using GCC. Returns the shared object as a string. */
std::string CppBackend::compile(std::string udf_file, std::string template_file)
{
    std::string bytecode;
    std::ifstream ifs(udf_file);
    if (! ifs.is_open())
    {
        fprintf(stderr, "Failed to open %s\n", udf_file.c_str());
        return "";
    }
    std::string inputFileBuffer(
		(std::istreambuf_iterator<char>(ifs)),
        (std::istreambuf_iterator<char>()  ));

    /* Basic check: does the template file exist? */
    if (template_file.size() == 0)
    {
        fprintf(stderr, "Failed to find C++ template file\n");
        return "";
    }
    std::ifstream ifstr(template_file);
    std::string udf(
		(std::istreambuf_iterator<char>(ifstr)),
        (std::istreambuf_iterator<char>()    ));

    /* Basic check: is the template string present in the template file? */
    std::string placeholder = "// user_callback_placeholder";
    auto start = udf.find(placeholder);
    if (start == std::string::npos)
    {
        fprintf(stderr, "Failed to find placeholder string in %s\n",
            template_file.c_str());
        return "";
    }

    /* Embed UDF string in the template */
    auto completeCode = udf.replace(start, placeholder.length(), inputFileBuffer);

    /* Compile the code */
    auto cpp_file = writeToDisk(completeCode.data(), completeCode.size(), ".cpp");
    if (cpp_file.size() == 0)
    {
        fprintf(stderr, "Will not be able to compile the UDF code\n");
        return "";
    }

    std::string output = udf_file + ".so";
    pid_t pid = fork();
    if (pid == 0)
    {
        // Child process
        char *cmd[] = {
            (char *) "g++",
            (char *) "-rdynamic",
            (char *) "-shared",
            (char *) "-fPIC",
            (char *) "-flto",
            (char *) "-Os",
            (char *) "-C",
            (char *) "-o",
            (char *) output.c_str(),
            (char *) cpp_file.c_str(),
            (char *) NULL
        };
        execvp(cmd[0], cmd);
    }
    else if (pid > 0)
    {
        // Parent
        int exit_status;
        wait4(pid, &exit_status, 0, NULL);

        struct stat statbuf;
        if (stat(output.c_str(), &statbuf) == 0) {
            std::ifstream data(output, std::ifstream::binary);
            std::vector<unsigned char> buffer(std::istreambuf_iterator<char>(data), {});
            bytecode.assign(buffer.begin(), buffer.end());
            unlink(output.c_str());
        }
        unlink(cpp_file.c_str());
        return bytecode;
    }
    fprintf(stderr, "Failed to execute g++\n");
    return bytecode;
}

/* Helper class to manage calls to dlopen and dlsym */
class SharedLibraryManager {
public:
    SharedLibraryManager(std::string _so_file) :
        so_file(_so_file),
        so_handle(NULL) { }

    ~SharedLibraryManager()
    {
        if (so_handle)
            dlclose(so_handle);
        unlink(so_file.c_str());
    }

    bool open()
    {
        (void) dlerror();
        so_handle = dlopen(so_file.c_str(), RTLD_NOW);
        if (! so_handle)
            fprintf(stderr, "Failed to load %s: %s\n", so_file.c_str(), dlerror());
        return so_handle != NULL;
    }

    void *loadsym(std::string name)
    {
        (void) dlerror();
        void *symbol = dlsym(so_handle, name.c_str());
        if (! symbol)
            fprintf(stderr, "%s\n", dlerror());
        return symbol;
    }
private:
    std::string so_file;
    void *so_handle;
};

/* Execute the user-defined-function embedded in the given buffer */
bool CppBackend::run(
    const std::string filterpath,
    const std::vector<DatasetInfo> input_datasets,
    const DatasetInfo output_dataset,
    const char *output_cast_datatype,
    const char *sharedlib_data,
    size_t sharedlib_data_size)
{
    /*
     * Unfortunately we have to make a trip to disk so we can dlopen()
     * and dlsym() the function we are looking for in a portable way.
     */
    auto so_file = writeToDisk(sharedlib_data, sharedlib_data_size, ".so");
    if (so_file.size() == 0)
    {
        fprintf(stderr, "Will not be able to load the UDF function\n");
        return false;
    }
    chmod(so_file.c_str(), 0755);

    SharedLibraryManager shlib(so_file);
    if (shlib.open() == false)
        return false;

    /* Get references to the UDF and the APIs defined in our C++ template file */
    void (*udf)(void) = (void (*)()) shlib.loadsym("dynamic_dataset");
    auto hdf5_udf_data =
        static_cast<std::vector<void *>*>(shlib.loadsym("hdf5_udf_data"));
    auto hdf5_udf_names =
        static_cast<std::vector<const char *>*>(shlib.loadsym("hdf5_udf_names"));
    auto hdf5_udf_types =
        static_cast<std::vector<const char *>*>(shlib.loadsym("hdf5_udf_types"));
    auto hdf5_udf_dims =
        static_cast<std::vector<std::vector<hsize_t>>*>(shlib.loadsym("hdf5_udf_dims"));
    if (! udf || ! hdf5_udf_data || ! hdf5_udf_names || ! hdf5_udf_types || ! hdf5_udf_dims)
        return false;

    /* Populate vector of dataset names, sizes, and types */
    std::vector<DatasetInfo> dataset_info;
    dataset_info.push_back(output_dataset);
    dataset_info.insert(
        dataset_info.end(), input_datasets.begin(), input_datasets.end());

    for (size_t i=0; i<dataset_info.size(); ++i)
    {
        hdf5_udf_data->push_back(dataset_info[i].data);
        hdf5_udf_names->push_back(dataset_info[i].name.c_str());
        hdf5_udf_types->push_back(dataset_info[i].getDatatype());
        hdf5_udf_dims->push_back(dataset_info[i].dimensions);
    }

    /* Execute the user-defined-function */
    udf();

    return true;   
}

/* Scan the UDF file for references to HDF5 dataset names */
std::vector<std::string> CppBackend::udfDatasetNames(std::string udf_file)
{
    std::vector<std::string> output;

    // We already rely on GCC to build the code, so just invoke its
    // preprocessor to get rid of comments and identify calls to our API
    int pipefd[2];
    if (pipe(pipefd) < 0)
    {
        fprintf(stderr, "Failed to create pipe\n");
        return output;
    }

    pid_t pid = fork();
    if (pid == 0)
    {
        // Child: runs proprocessor, outputs to pipe
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        char *cmd[] = {
            (char *) "g++",
            (char *) "-fpreprocessed",
            (char *) "-dD",
            (char *) "-E",
            (char *) udf_file.c_str(),
            (char *) NULL
        };
        execvp(cmd[0], cmd);
    }
    else if (pid > 0)
    {
        // Parent: reads from pipe, concatenating to 'input' string
        std::string input;
        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
        while (true)
        {
            char buf[8192];
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n < 0 && errno == EWOULDBLOCK)
            {
                if (waitpid(pid, NULL, WNOHANG) == pid)
                    break;
                continue;
            }
            else if (n <= 0)
                break;
            input.append(buf, n);
        }

        // Go through the output of the preprocessor one line at a time
        std::string line;
        std::istringstream iss(input);
        while (std::getline(iss, line))
        {
            size_t n = line.find("lib.getData");
            if (n != std::string::npos)
            {
                auto start = line.substr(n).find_first_of("\"");
                auto end = line.substr(n+start+1).find_first_of("\"");
                auto name = line.substr(n).substr(start+1, end);
                output.push_back(name);
            }
        }

        close(pipefd[0]);
        close(pipefd[1]);
    }
    return output;
}