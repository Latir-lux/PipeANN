set(CMAKE_CXX_STANDARD 17)

add_executable(build_memory_index build_memory_index.cpp)
target_link_libraries(build_memory_index ${PROJECT_NAME})

add_executable(search_memory_index search_memory_index.cpp)
target_link_libraries(search_memory_index ${PROJECT_NAME})

add_executable(build_disk_index build_disk_index.cpp)
target_link_libraries(build_disk_index ${PROJECT_NAME})

add_executable(gen_tags gen_tags.cpp)
target_link_libraries(gen_tags ${PROJECT_NAME})

add_executable(gt_update gt_update.cpp)
target_link_libraries(gt_update ${PROJECT_NAME} )

add_executable(change_pts change_pts.cpp)
target_link_libraries(change_pts ${PROJECT_NAME})

add_executable(overall_performance overall_performance.cpp)
target_link_libraries(overall_performance ${PROJECT_NAME})

add_executable(test_insert_search test_insert_search.cpp)
target_link_libraries(test_insert_search ${PROJECT_NAME})

add_executable(overall_perf_mem overall_perf_mem.cpp)
target_link_libraries(overall_perf_mem ${PROJECT_NAME})

add_executable(search_disk_index search_disk_index.cpp)
target_link_libraries(search_disk_index ${PROJECT_NAME})

add_executable(search_disk_index_mem search_disk_index_mem.cpp)
target_link_libraries(search_disk_index_mem ${PROJECT_NAME})

add_executable(pad_partition pad_partition.cpp)
target_link_libraries(pad_partition ${PROJECT_NAME} )

add_executable(normalize_data normalize_data.cpp)
target_link_libraries(normalize_data ${PROJECT_NAME})

# 论文第5章实验测试程序
add_executable(thesis_benchmark thesis_benchmark.cpp)
target_link_libraries(thesis_benchmark ${PROJECT_NAME})

# 系统对比测试程序 (DC-PDI vs IP-DiskANN vs FreshDiskANN)
add_executable(compare_systems compare_systems.cpp)
target_link_libraries(compare_systems ${PROJECT_NAME})

# 消融实验程序 (exp-chapter4and3-3)
# 实验1: 动态聚类消融 (拓扑感知分配 vs 随机分配)
# 实验2: 重组织机制消融 (有无后台重组织)
# 实验3: 流水线消融 (PIPE_SEARCH vs BEAM_SEARCH)
# 实验4: 并发扩展性测试
add_executable(ablation_study ablation_study.cpp)
target_link_libraries(ablation_study ${PROJECT_NAME})
