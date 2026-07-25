Feature: Responsiveness
  As a user
  I want the application to work at different terminal sizes
  So that I can use it comfortably on any display

  Scenario Outline: Terminal works at common sizes
    Given a board with no cards
    When I launch the application at <rows>x<cols>
    Then the screen should show the headers "To Do", "Doing", and "Done"
    And the application should still be running
    When I add a card "card-<rows>x<cols>" to "To Do"
    Then the "To Do" column should contain "card-<rows>x<cols>"

    Examples:
      | rows | cols |
      | 24   | 80   |
      | 40   | 120  |
