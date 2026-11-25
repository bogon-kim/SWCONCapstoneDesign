add_plugin(SampleCountPass)

target_sources(SampleCountPass PRIVATE
    SampleCountPass.cs.slang
    SampleCountPass.cpp
    SampleCountPass.h
    SppAllocation.cpp
    SppAllocation.h
)

# 소스의 한국어 주석을 UTF-8 로 읽게 한다 (ImportancePass 와 같은 이유: CP949 로 읽으면 주석 끝에서 줄바꿈이 사라진다).
target_compile_options(SampleCountPass PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

target_copy_shaders(SampleCountPass RenderPasses/SampleCountPass)

target_source_group(SampleCountPass "RenderPasses")
