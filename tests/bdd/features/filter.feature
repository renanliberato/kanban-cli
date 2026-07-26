Feature: Fuzzy Filter
  As a user
  I want to filter cards by title and description
  So that I can find relevant tasks in a large board

  Background:
    Given a board with the following cards
      | title         | column |
      | buy milk      | To Do  |
      | walk dog      | To Do  |
      | write code    | Doing  |
      | fix login     | Doing  |
      | deploy now    | Done   |

  Scenario: Filter narrows visible cards
    When I launch the application
    When I press "/"
    When I type "code"
    Then the header for "Doing" should show "1/2"

  Scenario: ESC clears the filter
    When I launch the application
    When I press "/"
    When I type "xyz"
    When I press ESC
    Then the header for "To Do" should show "2"
    And the header for "Doing" should show "2"
    And the header for "Done" should show "1"

  Scenario: Filter with Enter keeps it active
    When I launch the application
    When I press "/"
    When I type "buy"
    When I press Enter
    Then the header for "To Do" should show "1/2"
