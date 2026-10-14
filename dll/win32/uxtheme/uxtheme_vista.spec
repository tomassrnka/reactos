1 stdcall -noname QueryThemeServices()
2 stdcall -noname OpenThemeFile(wstr wstr wstr ptr long)
3 stdcall -noname CloseThemeFile(ptr)
4 stdcall -noname ApplyTheme(ptr ptr ptr)
37 stdcall -ordinal BeginBufferedAnimation(ptr ptr ptr long ptr ptr ptr ptr)
38 stdcall -ordinal BeginBufferedPaint(ptr ptr long ptr ptr)
7 stdcall -noname GetThemeDefaults(wstr wstr long wstr long)
8 stdcall -noname EnumThemes(wstr ptr ptr)
9 stdcall -noname EnumThemeColors(wstr wstr long ptr)
10 stdcall -noname EnumThemeSizes(wstr wstr long ptr)
11 stdcall -noname ParseThemeIniFile(wstr wstr ptr ptr)
39 stdcall -ordinal BufferedPaintClear(ptr ptr)
13 stdcall -noname DrawNCPreview(ptr long ptr wstr wstr wstr ptr ptr)
14 stub -noname RegisterDefaultTheme
15 stub -noname DumpLoadedThemeToTextFile
16 stdcall -noname OpenThemeDataFromFile(ptr ptr wstr long)
17 stub -noname OpenThemeFileFromData
18 stub -noname GetThemeSysSize96
19 stub -noname GetThemeSysFont96
20 stub -noname SessionAllocate
21 stub -noname SessionFree
22 stub -noname ThemeHooksOn
23 stub -noname ThemeHooksOff
24 stub -noname AreThemeHooksActive
25 stub -noname GetCurrentChangeNumber
26 stub -noname GetNewChangeNumber
27 stub -noname SetGlobalTheme
28 stub -noname GetGlobalTheme
29 stdcall -noname CheckThemeSignature(wstr)
30 stub -noname LoadTheme
31 stub -noname InitUserTheme
32 stub -noname InitUserRegistry
33 stub -noname ReestablishServerConnection
34 stdcall -noname ThemeHooksInstall()
35 stdcall -noname ThemeHooksRemove()
36 stub -noname RefreshThemeForTS
40 stdcall -ordinal BufferedPaintInit()
41 stdcall -ordinal BufferedPaintRenderAnimation(ptr ptr)
42 stdcall -ordinal BufferedPaintSetAlpha(ptr ptr long)
51 stdcall -ordinal BufferedPaintStopAllAnimations(ptr)
52 stdcall -ordinal BufferedPaintUnInit()
53 stdcall -ordinal CloseThemeData(ptr)
43 stdcall -noname ClassicGetSystemMetrics(long)
44 stdcall -noname ClassicSystemParametersInfoA(long long ptr long)
45 stdcall -noname ClassicSystemParametersInfoW(long long ptr long)
46 stdcall -noname ClassicAdjustWindowRectEx(ptr long long long)
47 stdcall -ordinal DrawThemeBackgroundEx(ptr ptr long long ptr ptr)
48 stdcall -noname GetThemeParseErrorInfo(ptr)
308 stdcall -stub -ordinal OpenNcThemeData()
307 stdcall -stub -ordinal IsThemeClassDefined(ptr ptr long long ptr ptr)
57 stdcall -ordinal DrawThemeBackground(ptr ptr long long ptr ptr)
58 stdcall -ordinal DrawThemeEdge(ptr ptr long long ptr long long ptr)
59 stdcall -ordinal DrawThemeIcon(ptr ptr long long ptr ptr long)
70 stdcall -ordinal DrawThemeParentBackground(ptr ptr ptr)
71 stub -ordinal DrawThemeParentBackgroundEx
89 stdcall -ordinal DrawThemeText(ptr ptr long long wstr long long long ptr)
103 stdcall -ordinal DrawThemeTextEx(ptr ptr long long wstr long long ptr ptr)
114 stdcall -ordinal EnableThemeDialogTexture(ptr long)
129 stdcall -ordinal EnableTheming(long)
60 stub -noname CreateThemeDataFromObjects
61 stdcall -noname OpenThemeDataEx(ptr wstr long)
62 stub -noname ServerClearStockObjects
63 stub -noname MarkSelection
309 stub -ordinal ProcessLoadTheme_RunDLLW
65 stdcall -noname SetSystemVisualStyle(wstr wstr wstr long)
312 stub -ordinal ServiceClearStockObjects
300 stdcall -stub -ordinal AddThemeAppCompatFlag(ptr long long long ptr)
311 stdcall -stub -ordinal ResetThemeAppCompatFlags(ptr ptr long long long ptr ptr)
303 stdcall -stub -ordinal EnumThemeProperties(ptr ptr long long long ptr)
134 stdcall -ordinal EndBufferedAnimation(ptr long)
140 stdcall -ordinal EndBufferedPaint(ptr long)
301 stdcall -stub -ordinal DrawThemeIconEx(ptr long long long ptr)
306 stub -ordinal IsThemeActiveByPolicy
305 stdcall -stub -ordinal GetThemeClass(ptr long long long ptr)
313 stdcall -stub -ordinal ThemeForwardCapturedMouseMessage(ptr long long long wstr long)
302 stdcall -stub -ordinal EnableServiceConnection(ptr long)
310 stdcall -stub -ordinal Remote_LoadTheme(ptr long) ; This exits a thread
141 stdcall -ordinal GetBufferedPaintBits(ptr ptr ptr)
142 stdcall -ordinal GetBufferedPaintDC(ptr)
143 stdcall -ordinal GetBufferedPaintTargetDC(ptr)
144 stdcall -ordinal GetBufferedPaintTargetRect(ptr ptr)
145 stdcall -ordinal GetCurrentThemeName(wstr long wstr long wstr long)
148 stdcall -ordinal GetThemeAppProperties()
149 stdcall -ordinal GetThemeBackgroundContentRect(ptr ptr long long ptr ptr)
150 stdcall -ordinal GetThemeBackgroundExtent(ptr ptr long long ptr ptr)
151 stdcall -ordinal GetThemeBackgroundRegion(ptr ptr long long ptr ptr)
304 stub -ordinal GetThemeBitMap
153 stdcall -ordinal GetThemeBool(ptr long long long ptr)
154 stdcall -ordinal GetThemeColor(ptr long long long ptr)
155 stdcall -ordinal GetThemeDocumentationProperty(wstr wstr wstr long)
156 stdcall -ordinal GetThemeEnumValue(ptr long long long ptr)
157 stdcall -ordinal GetThemeFilename(ptr long long long wstr long)
158 stdcall -ordinal GetThemeFont(ptr ptr long long long ptr)
159 stdcall -ordinal GetThemeInt(ptr long long long ptr)
160 stdcall -ordinal GetThemeIntList(ptr long long long ptr)
161 stdcall -ordinal GetThemeMargins(ptr ptr long long long ptr ptr)
162 stdcall -ordinal GetThemeMetric(ptr ptr long long long ptr)
163 stdcall -ordinal GetThemePartSize(ptr ptr long long ptr long ptr)
164 stdcall -ordinal GetThemePosition(ptr long long long ptr)
165 stdcall -ordinal GetThemePropertyOrigin(ptr long long long ptr)
166 stdcall -ordinal GetThemeRect(ptr long long long ptr)
167 stub -ordinal GetThemeStream
168 stdcall -ordinal GetThemeString(ptr long long long wstr long)
169 stdcall -ordinal GetThemeSysBool(ptr long)
170 stdcall -ordinal GetThemeSysColor(ptr long)
171 stdcall -ordinal GetThemeSysColorBrush(ptr long)
172 stdcall -ordinal GetThemeSysFont(ptr long ptr)
173 stdcall -ordinal GetThemeSysInt(ptr long ptr)
174 stdcall -ordinal GetThemeSysSize(ptr long)
175 stdcall -ordinal GetThemeSysString(ptr long wstr long)
176 stdcall -ordinal GetThemeTextExtent(ptr ptr long long wstr long long ptr ptr)
177 stdcall -ordinal GetThemeTextMetrics(ptr ptr long long ptr)
179 stdcall -ordinal GetThemeTransitionDuration(ptr long long long long ptr)
180 stdcall -ordinal GetWindowTheme(ptr)
181 stdcall -ordinal HitTestThemeBackground(ptr long long long long ptr long double ptr)
182 stdcall -ordinal IsAppThemed()
183 stdcall -ordinal IsCompositionActive()
184 stdcall -ordinal IsThemeActive()
185 stdcall -ordinal IsThemeBackgroundPartiallyTransparent(ptr long long)
186 stdcall -ordinal IsThemeDialogTextureEnabled(ptr)
187 stdcall -ordinal IsThemePartDefined(ptr long long)
188 stdcall -ordinal OpenThemeData(ptr wstr)
190 stdcall -ordinal SetThemeAppProperties(long)
191 stdcall -ordinal SetWindowTheme(ptr wstr wstr)
192 stdcall -ordinal SetWindowThemeAttribute(ptr long ptr long)
193 stdcall -ordinal ThemeInitApiHook(long ptr)

189 stdcall -ordinal OpenThemeDataForDpi(ptr wstr long) ;win7
5 stdcall -version=0x601+ -ordinal BeginPanningFeedback(ptr) ;win7
6 stdcall -version=0x601+ -ordinal EndPanningFeedback(ptr long) ;win7
12 stdcall -version=0x601+ -ordinal UpdatePanningFeedback(ptr long long long) ;win7
