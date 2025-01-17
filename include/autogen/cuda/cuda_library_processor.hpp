#pragma once

#include <filesystem>

#include "autogen/utils/stopwatch.hpp"
#include "autogen/utils/system.hpp"
#include "cuda_codegen.hpp"
#include "cuda_language.hpp"

namespace autogen {

template <class Base>
class CudaLibraryProcessor {
 protected:
  std::string nvcc_path_{"/usr/bin/nvcc"};
  int optimization_level_{0};

  /**
   * List of source file names to be created and included in the central library
   * file.
   */
  std::vector<std::string> gen_srcs_;

  /**
   * Directory where to store the source files.
   */
  mutable std::filesystem::path src_dir_;

  /**
   * Models to be contained whithin the library
   */
  std::list<CudaModelSourceGen<Base> *> models_;

  /**
   * Name of the library to be created.
   */
  std::string library_name_;

  std::vector<std::pair<std::string, std::string>> sources_;

  /**
   * Whether to compile the CUDA library with debug symbols.
   */
  bool debug_mode_{false};

 public:
  CudaLibraryProcessor(CudaModelSourceGen<Base> *model,
                       const std::string &library_name = "",
                       bool find_nvcc = true) {
    models_.push_back(model);
    if (library_name.empty()) {
      library_name_ = model->getName();
    } else {
      library_name_ = library_name;
    }
    if (find_nvcc) {
      nvcc_path_ = autogen::find_exe("nvcc");
    }
    if (nvcc_path_.size() < 3) {
      std::cerr << "NVIDIA CUDA Compiler (nvcc) could not be found. Make sure "
                   "\"nvcc\" is accessible from the system path.\n";
      std::exit(1);
    }
  }

  std::string &nvcc_path() { return nvcc_path_; }
  const std::string &nvcc_path() const { return nvcc_path_; }

  std::filesystem::path &src_dir() { return src_dir_; }
  const std::filesystem::path &src_dir() const { return src_dir_; }

  int &optimization_level() { return optimization_level_; }
  const int &optimization_level() const { return optimization_level_; }

  bool &debug_mode() { return debug_mode_; }
  const bool &debug_mode() const { return debug_mode_; }

  const std::vector<CudaModelSourceGen<Base> *> &models() const {
    return models_;
  }

  std::vector<std::pair<std::string, std::string>> &sources() {
    return sources_;
  }
  const std::vector<std::pair<std::string, std::string>> &sources() const {
    return sources_;
  }

  void add_model(CudaModelSourceGen<Base> *model, bool prepend = true) {
    if (prepend) {
      models_.push_front(model);
    } else {
      if (models_.empty()) {
        models_.push_back(model);
      } else {
        // insert just before the last element (since the last one is the main
        // model of the library)
        auto it = models_.end();
        std::advance(it, -1);
        models_.insert(it, model);
      }
    }
  }

  /**
   * Generates the CUDA kernels and (optional) kernel launch codes.
   * The source file contents can be accessed (and modified) via `sources()`.
   */
  void generate_code() {
    gen_srcs_.clear();
    LanguageCuda<Base>::add_debug_prints = debug_mode_;
    sources_.push_back(std::make_pair("util.h", util_header_src()));
    sources_.push_back(std::make_pair("model_info.h", model_info_header_src()));
    for (auto *cgen : models_) {
      std::string extension = cgen->is_kernel_only() ? "cuh" : "cu";
      if (cgen->isCreateForwardZero()) {
        std::string src_name = cgen->getName() + "_forward_zero." + extension;
        // generate CUDA code
        std::string source = cgen->forward_zero_source();
        sources_.push_back(std::make_pair(src_name, source));
        gen_srcs_.push_back(src_name);
      }
      if (cgen->isCreateSparseForwardOne()) {
        std::string src_name = cgen->getName() + "_forward_one." + extension;
        // generate CUDA code
        std::string source = cgen->forward_one_source(sources_);
        sources_.push_back(std::make_pair(src_name, source));
        gen_srcs_.push_back(src_name);
      }
      if (cgen->isCreateReverseOne()) {
        std::string src_name = cgen->getName() + "_reverse_one." + extension;
        // generate CUDA code
        std::string source = cgen->reverse_one_source(sources_);
        sources_.push_back(std::make_pair(src_name, source));
        gen_srcs_.push_back(src_name);
      }
      if (cgen->isCreateJacobian()) {
        std::string src_name = cgen->getName() + "_jacobian." + extension;
        // generate CUDA code
        std::string source = cgen->jacobian_source();
        sources_.push_back(std::make_pair(src_name, source));
        gen_srcs_.push_back(src_name);
      }
      if (cgen->isCreateSparseJacobian()) {
        std::string src_name =
            cgen->getName() + "_sparse_jacobian." + extension;
        // generate CUDA code
        std::string source = cgen->sparse_jacobian_source();
        sources_.push_back(std::make_pair(src_name, source));
        gen_srcs_.push_back(src_name);
      }
    }
    // generate "main" source file
    std::stringstream main_file;
    main_file << "#include \"util.h\"\n";
    main_file << "#include \"model_info.h\"\n\n";
    for (const auto &src : gen_srcs_) {
      main_file << "#include \"" << src << "\"\n";
    }
    sources_.push_back(std::make_pair(library_name_ + ".cu", main_file.str()));
  }

  /**
   * Saves the generated source files to the folder defined by `src_dir()`.
   */
  void save_sources() const {
    if (sources_.empty()) {
      throw std::runtime_error(
          "No source files have been generated yet. Ensure `generate_code()` "
          "is called before saving the code.");
    }
    namespace fs = std::filesystem;
    if (src_dir_.empty()) {
      src_dir_ = fs::path(library_name_ + "_srcs");
    }
    fs::create_directories(src_dir_);
    std::cout << "Saving source files at "
              << fs::canonical(fs::path(src_dir_)).u8string() << "\n";
    for (const auto &entry : sources_) {
      std::ofstream file(src_dir_ / entry.first);
      file << entry.second;
      file.close();
    }
  }

  /**
   * Compiles the previously generated code to a shared library file that can be
   * loaded subsequently.
   */
  void create_library() const {
    std::stringstream cmd;
    std::cout << "Compiling CUDA library via " << nvcc_path_ << std::endl;
    cmd << "\"" << nvcc_path_ << "\" ";
    cmd << "--ptxas-options=-O" << std::to_string(optimization_level_)
        << ",-v ";
    cmd << "--ptxas-options=-v "
        << "-rdc=true ";
    // if (debug_mode_) {
    //   cmd << "-G ";
    // }
#if AUTOGEN_SYSTEM_WIN
    cmd << "-o " << library_name_ << ".dll "
#else
    cmd << "--compiler-options "
        << "-fPIC "
        << "-o " << library_name_ << ".so "
#endif
        << "--shared ";
    cmd << (src_dir_ / (library_name_ + ".cu")).string();
    autogen::Stopwatch timer;
    std::cout << "\n\n" << cmd.str() << "\n\n";
    timer.start();
    int return_code = std::system(cmd.str().c_str());
    timer.stop();
    std::cout << "CUDA compilation process terminated after " << timer.elapsed()
              << " seconds.\n";
    if (return_code) {
      throw std::runtime_error("CUDA compilation failed with return code " +
                               std::to_string(return_code) + ".");
    }
  }

  std::string library_file_name() const {
#if AUTOGEN_SYSTEM_WIN
    return library_name_ + ".dll";
#else
    return library_name_ + ".so";
#endif
  }

 protected:
  std::string util_header_src() const {
    assert(!models_.empty());
    std::ostringstream code;
    code << "#ifndef CUDA_UTILS_H\n#define CUDA_UTILS_H\n\n";
    code << "#include <math.h>\n#include <stdio.h>\n\n";

    code << "typedef " << models_.front()->base_type_name() << " Float;\n\n";

    code << R"(#ifdef _WIN32
#define MODULE_API __declspec(dllexport)
#else
#define MODULE_API
#endif

struct CudaFunctionMetaData {
  int output_dim;
  int local_input_dim;
  int global_input_dim;
  bool accumulated_output;
};

void allocate(void **x, size_t size) {
  cudaError status = cudaMallocHost(x, size);
  if (status != cudaSuccess) {
    fprintf(stderr, "Error %i (%s) while allocating %zu units of CUDA memory: %s.\n",
            status, cudaGetErrorName(status), size, cudaGetErrorString(status));
    exit((int)status);
  }
}

#endif  // CUDA_UTILS_H)";
    return code.str();
  }

  std::string model_info_header_src() const {
    std::ostringstream code;
    code << "#ifndef MODEL_INFO_H\n#define MODEL_INFO_H\n\n";
    code << "extern \"C\" {\n";
    code << "MODULE_API void model_info(char const *const **names, int *count) "
            "{\n";
    code << "  static const char *const models[] = {\n";
    std::vector<std::string> accessible_kernels;
    for (const auto *cgen : models_) {
      if (!cgen->is_kernel_only()) {
        accessible_kernels.push_back(cgen->getName());
      }
    }
    for (std::size_t i = 0; i < accessible_kernels.size(); ++i) {
      code << "    \"" << accessible_kernels[i] << "\"";
      if (i < accessible_kernels.size() - 1) {
        code << ",";
      }
      code << "\n";
    }
    code << "  };\n";
    code << "  *names = models;\n";
    code << "  *count = " << accessible_kernels.size() << ";\n}\n";
    code << "}\n";
    code << "#endif  // MODEL_INFO_H\n";
    return code.str();
  }
};
}  // namespace autogen