#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>

#include "test.h"

// Counts the number of files in a given directory, excluding folders/subdirectories
int count_files(const std::string &directory_path) {
  int total_files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory_path)) {
    if (is_regular_file(entry.path())) {
      ++total_files;
    }
  }
  return total_files;
}

// Formats and generates an CSV file of collected metrics and results
void output_metrics_csv(const std::string &directory_path, 
                        const int total_files,
                        std::string filepaths[],
                        std::string durations[],
                        std::string video_streams[],
                        std::string fps[],
                        int compression_times[],
                        int decompression_times[],
                        double compression_rates[]) {
  // Redirect standard output to csv file
  freopen((directory_path + "/output/metrics.csv").c_str(), "w", stdout);
  // Print out columns
  std::cout << "File,Duration,Initial size (MB),Compressed size (MB),Compression rate (%),Space saving (%),Total time (ms),Compression time (ms),Compression speed (MB/s),Decompression time (ms),Decompression speed (MB/s),Video stream,Frames per second" << std::endl;
  int fail_count = 0;
  for (int i = 0; i < total_files; i++) {
    // Count the number of failures from the results
    if (compression_rates[i] < 0) {
      ++fail_count;
      continue;
    }
    
    // Calculate the original size of the video in MB
    double original_size = std::filesystem::file_size(filepaths[i]) / 1000000.0;
    // Print out results and metrics for video
    std::cout << "\"" + filepaths[i] + "\"" << "," 
              << durations[i] << "," 
              << original_size << ","
              << original_size * (compression_rates[i] / 100) << ","
              << compression_rates[i] << ","
              << 100 - compression_rates[i] << ","
              << compression_times[i] + decompression_times[i] << ","
              << compression_times[i] << ","
              << original_size / (compression_times[i] / 1000.0) << ","
              << decompression_times[i] << ","
              << original_size / (decompression_times[i] / 1000.0) << ","
              << video_streams[i] << ","
              << fps[i] << std::endl;
  }
  // Redirect output back to command line console
  freopen("/dev/tty", "w", stdout);

  // Output failure count to console
  if (fail_count > 0) {
    std::cout << "Compress-decompress roundtrip failed on " << fail_count << " / " << total_files << " files" << std::endl;
  }
}

// Parses through the text file of runtime results/output in output subdirectory to collect for additional metrics
void parse_collect_metrics(const std::string &directory_path, const int total_files, int compression_times[], int decompression_times[]) {
  std::string filepaths[total_files]; // Stores the filepath of every video
  std::string durations[total_files]; // Stores the duration of every video
  std::string video_streams[total_files]; // Stores information about the stream on each video
  std::string fps[total_files]; // Stores the frames per second (FPS) on each video
  double compression_rates[total_files]; // Stores the resulting compressing rates on each video

  int index = -1; // Keeps track of which video we are collecting metrics on
  std::string line;
  std::ifstream log_output(directory_path + "/output/log.txt");
  if (log_output.is_open()) {
    while (getline(log_output, line)) {
      int substr_len;
      if (line.find("Input #") != std::string::npos && line.find("from '") != std::string::npos) { 
        // Update video count accordingly
        ++index; 
        // Mark initially unsuccessful
        compression_rates[index] = -1; 
        // Extract filename
        substr_len = line.find_last_of("'") - line.find_first_of("'") - 1;
        filepaths[index] = line.substr(line.find_first_of("'") + 1, substr_len);
      } else if (line.find("Duration: ") != std::string::npos) { 
        // Extract duration
        substr_len = line.find_first_of(",") - line.find_first_of(":") - 2;
        durations[index] = line.substr(line.find_first_of(":") + 2, substr_len);
      } else if (line.find("Stream ") != std::string::npos && line.find(":0(") != std::string::npos && line.find("Video: ") != std::string::npos) { 
        // Extract video stream and FPS
        substr_len = line.find_first_of(",") - line.find("Video: ") - 7;
        video_streams[index] = line.substr(line.find("Video: ") + 7, substr_len);
        substr_len = line.find("fps") - line.find("/s, ") - 5;
        fps[index] = line.substr(line.find("/s, ") + 4, substr_len);
      } else if (line.find("compression ratio: ") != std::string::npos) { 
        // Extract compression ratio
        substr_len = line.find("%") - line.find(":") - 2;
        // Mark sucessful if compression ratio was found in output and then record
        compression_rates[index] = std::stod(line.substr(line.find(":") + 2, substr_len)); 
      }
    }
    log_output.close();
  }

  output_metrics_csv(directory_path, total_files, filepaths, durations, video_streams, fps, compression_times, decompression_times, compression_rates);
}

// Iteratively runs avrecode roundtrip on each file in the test directory saving the results and output to a subdirectory
void perf_test_driver(const std::string &directory_path, int (*roundtrip)(const std::string&, std::ostream*, int*, int*, const int)) {
  const int total_files = count_files(directory_path);

  // Create output subdirectory within test directory for decompressed output files and logs/metrics
  std::filesystem::create_directory(directory_path + "/output");

  // Arrays to store the times for compressing and decompressing on each file
  int compression_times[total_files];
  int decompression_times[total_files];

  int file_count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(directory_path)) {
    if (is_regular_file(entry.path())) {
      ++file_count;
      std::cout << file_count << "/" << total_files << "..." << std::endl; // Prints out progress on videos

      freopen((directory_path + "/output/log.txt").c_str(), file_count < 2 ? "w" : "a", stderr); // Open up text file to send command line output from running avrecode
      try {
        std::string filepath = entry.path().string();
        std::ofstream output_file(directory_path + "/output/" + filepath.substr(filepath.find_last_of('/') + 1)); // Sending the decompressed file from roundtrip to the output folder
        
        roundtrip(filepath, 
                  output_file.is_open() ? &output_file : nullptr,  
                  &compression_times[file_count - 1], 
                  &decompression_times[file_count - 1],
                  file_count - 1);
      } catch (const std::exception& e) {
        std::cerr << "Exception (" << typeid(e).name() << "): " << e.what() << std::endl;
      }
      std::cerr << std::endl;
      freopen("/dev/tty", "w", stderr); // Redirect standard output back to command line console
    }
  }

  parse_collect_metrics(directory_path, total_files, compression_times, decompression_times);
}





