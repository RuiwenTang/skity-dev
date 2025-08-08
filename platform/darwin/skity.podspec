Pod::Spec.new do |s|
    s.name         = "skity"
    s.version      =  "1.0.0-rc.0"
    s.summary      = "Skity is a 2D graphics library written in C++."
    s.homepage     = "https://github.com/lynx-family/skity"
    s.license      = { 
        :type => "Apache License, Version 2.0", 
        :file => "LICENSE"
    }
    s.author       = { "RuiwenTang" => "tangruiwen.mmh1103@bytedance.com" }

    s.source = {
        :http => "https://github.com/RuiwenTang/skity-dev/releases/download/v1.0.0-rc.0/skity.framework-v1.0.0-rc.0.zip",
    }

    s.platforms = {
        :ios => "12.0",
        :osx => "15.0"
    }

    s.vendored_frameworks = "skity.xcframework"
    s.static_framework = true
    s.ios.frameworks = "CoreText", "CoreGraphics", "Metal", "Foundation"
    s.osx.frameworks = "CoreText", "CoreGraphics", "Metal", "Foundation"

end