# Definimos Shared como una librería de solo cabeceras (INTERFACE)
add_library(MiddlewareShared INTERFACE)

# Indicamos dónde están las cabeceras para que otros las encuentren
target_include_directories(MiddlewareShared INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})