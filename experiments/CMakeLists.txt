# Exploratory microarchitecture experiments, kept separate from
# benchmarks/ because they talk to raw liburing instead of exercising
# ringio's own classes -- see sqpoll_core_budget.cpp for why.

add_executable(sqpoll_core_budget sqpoll_core_budget.cpp)
target_link_libraries(sqpoll_core_budget PRIVATE ringio::ringio)
