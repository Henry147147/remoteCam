#!/usr/bin/env ruby
# frozen_string_literal: true

require 'xcodeproj'
require 'fileutils'

root = File.expand_path('..', __dir__)
project_path = File.join(root, 'RemoteCam.xcodeproj')
FileUtils.rm_rf(project_path)

project = Xcodeproj::Project.new(project_path)
project.root_object.attributes['LastSwiftUpdateCheck'] = '2600'
project.root_object.attributes['LastUpgradeCheck'] = '2600'

app = project.new_target(:application, 'RemoteCam', :ios, '17.0')
tests = project.new_target(:unit_test_bundle, 'RemoteCamTests', :ios, '17.0')
activity = project.new_target(:app_extension, 'RemoteCamActivity', :ios, '17.0')
app.add_dependency(activity)

sources_group = project.main_group.new_group('RemoteCam')
Dir.glob(File.join(root, 'RemoteCam/Sources/**/*.swift')).sort.each do |path|
  reference = sources_group.new_file(path.sub(root + '/', ''))
  app.add_file_references([reference])
end
assets = sources_group.new_file('RemoteCam/Resources/Assets.xcassets')
app.resources_build_phase.add_file_reference(assets)
privacy_manifest = sources_group.new_file('RemoteCam/Resources/PrivacyInfo.xcprivacy')
app.resources_build_phase.add_file_reference(privacy_manifest)

tests_group = project.main_group.new_group('RemoteCamTests')
Dir.glob(File.join(root, 'RemoteCamTests/**/*.swift')).sort.each do |path|
  reference = tests_group.new_file(path.sub(root + '/', ''))
  tests.add_file_references([reference])
end

activity_group = project.main_group.new_group('RemoteCamActivity')
activity_sources = Dir.glob(File.join(root, 'RemoteCamActivity/**/*.swift')).sort + [
  File.join(root, 'RemoteCam/Sources/Activity/RemoteCamActivityAttributes.swift')
]
activity_sources.each do |path|
  reference = activity_group.new_file(path.sub(root + '/', ''))
  activity.add_file_references([reference])
end

# Keep protocol/storage tests independent of an application host. This makes the
# suite reliable on CI and avoids launching a camera-capable app in the simulator.
shared_test_sources = Dir.glob(File.join(root, 'RemoteCam/Sources/{Models,Storage,Wire}/**/*.swift')) + [
  File.join(root, 'RemoteCam/Sources/Capture/VideoEncoder.swift')
]
shared_test_sources.sort.each do |path|
  reference = tests_group.new_file(path.sub(root + '/', ''))
  tests.add_file_references([reference])
end

app.build_configurations.each do |config|
  config.build_settings.merge!({
    'ASSETCATALOG_COMPILER_APPICON_NAME' => 'AppIcon',
    'ASSETCATALOG_COMPILER_GLOBAL_ACCENT_COLOR_NAME' => 'AccentColor',
    'ASSETCATALOG_COMPILER_GENERATE_SWIFT_ASSET_SYMBOL_EXTENSIONS' => 'YES',
    'CODE_SIGN_STYLE' => 'Automatic',
    'CURRENT_PROJECT_VERSION' => '1',
    'GENERATE_INFOPLIST_FILE' => 'NO',
    'INFOPLIST_FILE' => 'RemoteCam/Configuration/Info.plist',
    'IPHONEOS_DEPLOYMENT_TARGET' => '17.0',
    'MARKETING_VERSION' => '1.0.0',
    'PRODUCT_BUNDLE_IDENTIFIER' => 'org.remotecam.ios',
    'PRODUCT_NAME' => '$(TARGET_NAME)',
    'SWIFT_STRICT_CONCURRENCY' => 'complete',
    'SWIFT_VERSION' => '6.0',
    'TARGETED_DEVICE_FAMILY' => '1,2'
  })
end

tests.build_configurations.each do |config|
  config.build_settings.merge!({
    'CODE_SIGN_STYLE' => 'Automatic',
    'GENERATE_INFOPLIST_FILE' => 'YES',
    'IPHONEOS_DEPLOYMENT_TARGET' => '17.0',
    'PRODUCT_BUNDLE_IDENTIFIER' => 'org.remotecam.ios.tests',
    'SWIFT_STRICT_CONCURRENCY' => 'complete',
    'SWIFT_VERSION' => '6.0',
    'TARGETED_DEVICE_FAMILY' => '1,2'
  })
end

activity.build_configurations.each do |config|
  config.build_settings.merge!({
    'APPLICATION_EXTENSION_API_ONLY' => 'YES',
    'CODE_SIGN_STYLE' => 'Automatic',
    'CURRENT_PROJECT_VERSION' => '1',
    'GENERATE_INFOPLIST_FILE' => 'NO',
    'INFOPLIST_FILE' => 'RemoteCamActivity/Info.plist',
    'IPHONEOS_DEPLOYMENT_TARGET' => '17.0',
    'MARKETING_VERSION' => '1.0.0',
    'PRODUCT_BUNDLE_IDENTIFIER' => 'org.remotecam.ios.activity',
    'PRODUCT_NAME' => '$(TARGET_NAME)',
    'SKIP_INSTALL' => 'YES',
    'SWIFT_STRICT_CONCURRENCY' => 'complete',
    'SWIFT_VERSION' => '6.0',
    'TARGETED_DEVICE_FAMILY' => '1,2'
  })
end

embed_extensions = app.new_copy_files_build_phase('Embed App Extensions')
embed_extensions.dst_subfolder_spec = '13'
embed_extensions.add_file_reference(activity.product_reference)

project.save

scheme = Xcodeproj::XCScheme.new
scheme.add_build_target(app)
scheme.add_test_target(tests)
scheme.save_as(project_path, 'RemoteCam', true)

puts "Generated #{project_path}"
