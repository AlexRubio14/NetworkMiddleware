
add_library(Brain STATIC
        BrainManager.cpp
        BrainManager.h
        NeuralProcessor.cpp
        NeuralProcessor.h
        BehaviorTree.cpp
        BehaviorTree.h
        Interfaces/IDataProcessor.h
        Interfaces/IBehaviorEngine.h
        KalmanPredictor.cpp
        KalmanPredictor.h
)

target_include_directories(Brain PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})