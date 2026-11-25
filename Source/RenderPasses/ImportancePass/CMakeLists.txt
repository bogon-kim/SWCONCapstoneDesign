add_plugin(ImportancePass)

target_sources(ImportancePass PRIVATE
    ImportancePass.cs.slang
    ImportancePass.cpp
    ImportancePass.h
    TaskClassifier.cpp
    TaskClassifier.h
    TagMapping.cpp
    TagMapping.h
    TaskRoi.cpp
    TaskRoi.h
    EnemyField.cpp
    EnemyField.h
    TagPass.cs.slang
    EnemyField.cs.slang
)

# 소스의 한국어 주석을 UTF-8 로 읽게 한다. 없으면 CP949 로 해석되어 주석 끝 글자가 줄바꿈을 삼키고
# 다음 줄 코드가 주석 처리될 수 있다.
target_compile_options(ImportancePass PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/utf-8>)

target_copy_shaders(ImportancePass RenderPasses/ImportancePass)

target_source_group(ImportancePass "RenderPasses")
