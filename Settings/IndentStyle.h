
#ifndef INDENT_STYLE_H_
#define INDENT_STYLE_H_

#include <QLatin1String>
#include <QtDebug>

enum class IndentStyle : int {
	Default = -1,
	None    = 0,
	Auto    = 1,
	Smart   = 2
};

template <class T>
inline T from_integer(int value);

template <>
inline IndentStyle from_integer(int value) {
	switch(value) {
	case static_cast<int>(IndentStyle::Default):
	case static_cast<int>(IndentStyle::None):
	case static_cast<int>(IndentStyle::Auto):
	case static_cast<int>(IndentStyle::Smart):
		return static_cast<IndentStyle>(value);
	default:
		qWarning("NEdit: Invalid value for IndentStyle");
		return IndentStyle::Default;
	}
}

inline constexpr QLatin1String to_string(IndentStyle style) {

	switch(style) {
	case IndentStyle::None:
		return QLatin1String("off");
	case IndentStyle::Auto:
		return QLatin1String("on");
	case IndentStyle::Smart:
		return QLatin1String("smart");
	case IndentStyle::Default:
		return QLatin1String("default");
	}

	Q_UNREACHABLE();
}

#endif
