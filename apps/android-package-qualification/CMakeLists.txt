# Native-free application composition shared by the real Android host and its protocol tests.
add_library(HoroAndroidPresentationAdmissionInternal STATIC PresentationAdmission.cpp)
target_compile_features(HoroAndroidPresentationAdmissionInternal PUBLIC cxx_std_20)
set_target_properties(HoroAndroidPresentationAdmissionInternal PROPERTIES POSITION_INDEPENDENT_CODE ON)
set(_horo_android_admission_headers "${CMAKE_CURRENT_BINARY_DIR}/internal/android-presentation-admission")
file(MAKE_DIRECTORY "${_horo_android_admission_headers}/Horo/Apps/AndroidQualification")
configure_file(PresentationAdmission.h
    "${_horo_android_admission_headers}/Horo/Apps/AndroidQualification/PresentationAdmission.h" COPYONLY)
target_include_directories(HoroAndroidPresentationAdmissionInternal PUBLIC "${_horo_android_admission_headers}")
target_link_libraries(HoroAndroidPresentationAdmissionInternal PUBLIC HoroPlatformAndroidWindowInternal)
