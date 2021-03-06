# ==========================================
#   Unity Project - A Test Framework for C
#   Copyright (c) 2007 Mike Karlesky, Mark VanderVoord, Greg Williams
#   [Released under MIT License. Please refer to license.txt for details]
# ========================================== 

HERE = File.expand_path(File.dirname(__FILE__)) + '/'

require 'rake'
require 'rake/clean'
require 'rake/testtask'
require HERE + 'rakefile_helper'

include RakefileHelpers

# Load default configuration, for now
DEFAULT_CONFIG_FILE = 'gcc_32.yml'
configure_toolchain(DEFAULT_CONFIG_FILE)

task :unit do
  run_tests
end

desc "Build and test Unity Framework"
task :all => [:clean, :unit]
task :default => [:clobber, :all]
task :ci => [:no_color, :default]
task :cruise => [:no_color, :default]

desc "Load configuration"
task :config, :config_file do |t, args|
  configure_toolchain(args[:config_file])
end

task :no_color do
  $colour_output = false
end
