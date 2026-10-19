project(tests)

ecm_add_test(FileSystem_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME FileSystem)

ecm_add_test(GZip_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME GZip)

ecm_add_test(GradleSpecifier_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME GradleSpecifier)

ecm_add_test(MojangVersionFormat_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME MojangVersionFormat)

ecm_add_test(Library_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Library)

ecm_add_test(ResourceFolderModel_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ResourceFolderModel)

ecm_add_test(ResourcePackParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ResourcePackParse)

ecm_add_test(TexturePackParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME TexturePackParse)

ecm_add_test(DataPackParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME DataPackParse)

ecm_add_test(ShaderPackParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ShaderPackParse)

ecm_add_test(WorldSaveParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME WorldSaveParse)

ecm_add_test(ParseUtils_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ParseUtils)

ecm_add_test(QuickplayPreset_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME QuickplayPreset)

ecm_add_test(QuickplayLoader_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME QuickplayLoader)

ecm_add_test(QuickplayResolver_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME QuickplayResolver)

ecm_add_test(QuickplayPack_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME QuickplayPack)

ecm_add_test(ProjectDescription_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME ProjectDescription)

ecm_add_test(MarkdownTables_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME MarkdownTables)

ecm_add_test(PackGallery_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME PackGallery)

ecm_add_test(PlaytimeBreakdown_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME PlaytimeBreakdown)

ecm_add_test(PlaytimeDetails_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME PlaytimeDetails)

ecm_add_test(PlaytimeLedger_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME PlaytimeLedger)

ecm_add_test(SharedPack_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test  # pickle-sharedpack
    TEST_NAME SharedPack)

ecm_add_test(SharedPackInvite_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test  # pickle-sharedpack
    TEST_NAME SharedPackInvite)

ecm_add_test(SharedPackApply_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test  # pickle-sharedpack
    TEST_NAME SharedPackApply)

ecm_add_test(SharedPackTransfer_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test  # pickle-sharedpack
    TEST_NAME SharedPackTransfer)

ecm_add_test(SharedPackLink_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test  # pickle-sharedpack
    TEST_NAME SharedPackLink)

ecm_add_test(SharedPackPublish_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test  # pickle-sharedpack
    TEST_NAME SharedPackPublish)

ecm_add_test(SharedPackTarget_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test  # pickle-sharedpack
    TEST_NAME SharedPackTarget)

ecm_add_test(SharedPackSelection_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test  # pickle-sharedpack
    TEST_NAME SharedPackSelection)

ecm_add_test(Task_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Task)

ecm_add_test(INIFile_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME INIFile)

ecm_add_test(JavaVersion_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME JavaVersion)

ecm_add_test(Packwiz_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Packwiz)

ecm_add_test(Index_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Index)

ecm_add_test(Version_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME Version)

ecm_add_test(MetaComponentParse_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME MetaComponentParse)

ecm_add_test(CatPack_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME CatPack)


ecm_add_test(XmlLogs_test.cpp LINK_LIBRARIES Launcher_logic Qt${QT_VERSION_MAJOR}::Test
    TEST_NAME XmlLogs)
