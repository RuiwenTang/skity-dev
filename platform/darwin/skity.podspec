Pod::Spec.new do |s|
    lib_version = "1.0.0"

    s.name         = "skity"
    s.version      = lib_version
    s.summary      = "Skity is a 2D graphics library written in C++."
    s.homepage     = "https://github.com/lynx-family/skity"
    s.license      = { 
        :type => "Apache License, Version 2.0", 
        :file => "../../LICENSE"
    }
    s.author       = { "RuiwenTang" => "tangruiwen.mmh1103@bytedance.com" }

    s.source = {
        :http => "https://github.com/lynx-family/skity/releases/download/v#{lib_version}/skity.framework-v#{lib_version}.zip"
    }

    s.platforms = {
        :ios => "11.0",
        :osx => "15.0"
    }

    s.vendored_frameworks = "skity.framework"
    s.static_framework = true
    s.ios.frameworks = "CoreText", "CoreGraphics", "Metal", "Foundation"
    s.osx.frameworks = "CoreText", "CoreGraphics", "Metal", "Foundation"

end