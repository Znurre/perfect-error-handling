#include <coroutine>
#include <expected>
#include <iostream>
#include <memory>

template <typename T>
struct promise;

template <typename T>
struct result
{
	using promise_type = promise<T>;

	result(std::shared_ptr<std::expected<T, uint64_t>> value)
		: value_(value)
	{
	}

	T await_resume()
	{
		return **value_;
	}

	template <typename U>
	bool await_suspend(std::coroutine_handle<promise<U>> handle)
	{
		if (!*value_)
		{
			auto& promise = handle.promise();

			promise.return_value(std::unexpected(value_->error()));

			handle.destroy();

			return true;
		}

		return false;
	}

	bool await_ready()
	{
		return false;
	}

	operator bool() const
	{
		return value_->has_value();
	}

	T& operator*() const
	{
		return value_->value();
	}

	uint64_t error() const
	{
		return value_->error();
	}

private:
	std::shared_ptr<std::expected<T, uint64_t>> value_;
};

template <typename T>
struct promise
{
	promise()
		: value_(std::make_shared<std::expected<T, uint64_t>>())
	{
	}

	result<T> get_return_object()
	{
		return value_;
	}

	std::suspend_never initial_suspend() noexcept
	{
		return {};
	}

	std::suspend_never final_suspend() noexcept
	{
		return {};
	}

	void unhandled_exception()
	{
	}

	void return_value(std::expected<T, uint64_t>&& value)
	{
		*value_ = value;
	}

private:
	std::shared_ptr<std::expected<T, uint64_t>> value_;
};

result<FILE*> open_file(std::string_view file_name, std::string_view mode)
{
	if (auto file = fopen(file_name.data(), mode.data()))
	{
		co_return file;
	}

	co_return std::unexpected(errno);
}

result<std::string> read_from_file(std::string_view file_name)
{
	auto file = co_await open_file(file_name, "r");

	std::string target;

	char buffer[4096];

	for (auto size = 0; (size = fread(buffer, sizeof(*buffer), sizeof(buffer), file)) > 0;)
	{
		target.append(buffer, buffer + size);
	}

	if (auto error = ferror(file))
	{
		co_return std::unexpected(error);
	}

	co_return target;
}

int main()
{
	if (auto result = read_from_file("existing_file.txt"))
	{
		std::cout << *result << std::endl;
	}
	else
	{
		std::cout << "Failed to read file with error " << result.error() << std::endl;
	}

	return 0;
}
